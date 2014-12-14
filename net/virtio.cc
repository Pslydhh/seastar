/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "virtio.hh"
#include "core/posix.hh"
#include "core/future-util.hh"
#include "core/vla.hh"
#include "virtio-interface.hh"
#include "core/reactor.hh"
#include "core/stream.hh"
#include "core/circular_buffer.hh"
#include "core/align.hh"
#include "util/function_input_iterator.hh"
#include "util/transform_iterator.hh"
#include <atomic>
#include <vector>
#include <queue>
#include <fcntl.h>
#include <linux/vhost.h>
#include <linux/if_tun.h>
#include "ip.hh"
#include "const.hh"
#include "net/proxy.hh"
#include "net/native-stack.hh"

#ifdef HAVE_OSV
#include <osv/virtio-assign.hh>
#endif

using namespace net;

namespace virtio {

class device : public net::device {
private:
    boost::program_options::variables_map _opts;
    net::hw_features _hw_features;
    uint64_t _features;

private:
    uint64_t setup_features() {
        int64_t seastar_supported_features = VIRTIO_RING_F_INDIRECT_DESC | VIRTIO_NET_F_MRG_RXBUF;

        if (!(_opts.count("event-index") && _opts["event-index"].as<std::string>() == "off")) {
            seastar_supported_features |= VIRTIO_RING_F_EVENT_IDX;
        }
        if (!(_opts.count("csum-offload") && _opts["csum-offload"].as<std::string>() == "off")) {
            seastar_supported_features |= VIRTIO_NET_F_CSUM | VIRTIO_NET_F_GUEST_CSUM;
            _hw_features.tx_csum_l4_offload = true;
            _hw_features.rx_csum_offload = true;
        } else {
            _hw_features.tx_csum_l4_offload = false;
            _hw_features.rx_csum_offload = false;
        }
        if (!(_opts.count("tso") && _opts["tso"].as<std::string>() == "off")) {
            seastar_supported_features |= VIRTIO_NET_F_HOST_TSO4;
            seastar_supported_features |= VIRTIO_NET_F_GUEST_TSO4;
            _hw_features.tx_tso = true;
        } else {
            _hw_features.tx_tso = false;
        }
        if (!(_opts.count("ufo") && _opts["ufo"].as<std::string>() == "off")) {
            seastar_supported_features |= VIRTIO_NET_F_HOST_UFO;
            seastar_supported_features |= VIRTIO_NET_F_GUEST_UFO;
            _hw_features.tx_ufo = true;
        } else {
            _hw_features.tx_ufo = false;
        }

        seastar_supported_features |= VIRTIO_NET_F_MAC;
        return seastar_supported_features;
    }

public:
    device(boost::program_options::variables_map opts)
       : _opts(opts), _features(setup_features())
       {}
    ethernet_address hw_address() override {
        return { 0x12, 0x23, 0x34, 0x56, 0x67, 0x78 };
    }

    net::hw_features hw_features() {
        return _hw_features;
    }

    uint64_t features() {
        return _features;
    }

    virtual void init_local_queue(boost::program_options::variables_map opts) override;
};

/* The virtio_notifier class determines how to do host-to-guest and guest-to-
 * host notifications. We have two different implementations - one for vhost
 * (where both notifications occur through eventfds) and one for an assigned
 * virtio device from OSv.
 */
class notifier {
public:
    // Notify the host
    virtual void notify() = 0;
    // Wait for the host to notify us
    virtual future<> wait() = 0;
    // Do whatever it takes to wake wait(). A notifier does not need to
    // implement this function if wait() waits for an external even which is
    // generated by an external process (e.g., virtio_notifier_host doesn't
    // need to implement this).
    virtual void wake_wait() {
        abort();
    }
    virtual ~notifier() {
    }
};

class notifier_vhost : public notifier {
private:
    readable_eventfd _notified;
    writeable_eventfd _kick;
public:
    virtual void notify() override {
        _kick.signal(1);
    }
    virtual future<> wait() override {
        // convert _notified.wait(), a future<size_t>, to a future<>:
        return _notified.wait().then([this] (size_t ignore) {
            return make_ready_future<>();
        });
    }
    notifier_vhost(readable_eventfd &&notified,
            writeable_eventfd &&kick)
        : _notified(std::move(notified))
        , _kick(std::move(kick)) {}
};

#ifdef HAVE_OSV
class notifier_osv : public notifier {
private:
    std::unique_ptr<reactor_notifier> _notified;
    uint16_t _q_index;
    osv::assigned_virtio &_virtio;
public:
    virtual void notify() override {
        _virtio.kick(_q_index);
    }
    virtual future<> wait() override {
        return _notified->wait();
    }
    virtual void wake_wait() override {
        _notified->signal();
    }
    notifier_osv(osv::assigned_virtio &virtio, uint16_t q_index)
        : _notified(engine.make_reactor_notifier())
        , _q_index(q_index)
        , _virtio(virtio)
    {
    }
};
#endif

using phys = uint64_t;

// The 'buffer_chain' concept, used in vring, is a container of vring::buffer
// with an added 'promise<> completed' member, as in:
//
//   struct buffer_chain : std::vector<buffer> {
//      promise<size_t> completed;
//   };
class vring {
public:
    struct config {
        char* descs;
        char* avail;
        char* used;
        unsigned size;
        bool event_index;
        bool indirect;
        bool mergable_buffers;
    };
    struct buffer {
        phys addr;
        uint32_t len;
        bool writeable;
    };
private:
    class desc {
    public:
        struct flags {
            // This marks a buffer as continuing via the next field.
            uint16_t has_next : 1;
            // This marks a buffer as write-only (otherwise read-only).
            uint16_t writeable : 1;
            // This means the buffer contains a list of buffer descriptors.
            uint16_t indirect : 1;
        };

        phys get_paddr();
        uint32_t get_len() { return _len; }
        uint16_t next_idx() { return _next; }

        phys _paddr;
        uint32_t _len;
        flags _flags;
        uint16_t _next;
    };

    // Guest to host
    struct avail_layout {
        struct flags {
            // Mark that we do not need an interrupt for consuming a descriptor
            // from the ring. Unreliable so it's simply an optimization
            uint16_t no_interrupts : 1;
        };

        std::atomic<uint16_t> _flags;

        // Where we put the next descriptor
        std::atomic<uint16_t> _idx;
        // There may be no more entries than the queue size read from device
        uint16_t _ring[];
        // used event index is an optimization in order to get an interrupt from the host
        // only when the value reaches this number
        // The location of this field is places after the variable length ring array,
        // that's why we cannot fully define it within the struct and use a function accessor
        //std::atomic<uint16_t> used_event;
    };

    struct used_elem {
        // Index of start of used _desc chain. (uint32_t for padding reasons)
        uint32_t _id;
        // Total length of the descriptor chain which was used (written to)
        uint32_t _len;
    };

    // Host to guest
    struct used_layout {
        enum {
            // The Host advise the Guest: don't kick me when
            // you add a buffer.  It's unreliable, so it's simply an
            // optimization. Guest will still kick if it's out of buffers.
            no_notify = 1
        };

        // Using std::atomic since it being changed by the host
        std::atomic<uint16_t> _flags;
        // Using std::atomic in order to have memory barriers for it
        std::atomic<uint16_t> _idx;
        used_elem _used_elements[];
        // avail event index is an optimization kick the host only when the value reaches this number
        // The location of this field is places after the variable length ring array,
        // that's why we cannot fully define it within the struct and use a function accessor
        //std::atomic<uint16_t> avail_event;
    };

    struct avail {
        explicit avail(config conf);
        avail_layout* _shared;
        uint16_t _head = 0;
        uint16_t _avail_added_since_kick = 0;
    };
    struct used {
        explicit used(config conf);
        used_layout* _shared;
        uint16_t _tail = 0;
    };
private:
    config _config;
    std::unique_ptr<notifier> _notifier;
    std::unique_ptr<promise<size_t>[]> _completions;
    desc* _descs;
    avail _avail;
    used _used;
    std::atomic<uint16_t>* _avail_event;
    std::atomic<uint16_t>* _used_event;
    semaphore _available_descriptors = { 0 };
    int _free_head = -1;
    int _free_last = -1;
    std::vector<uint16_t> _batch;
    std::experimental::optional<reactor::poller> _poller;
    bool _poll_mode = false;
public:

    explicit vring(config conf, bool poll_mode);
    void set_notifier(std::unique_ptr<notifier> notifier) {
        _notifier = std::move(notifier);
    }
    const config& getconfig() {
        return _config;
    }
    void wake_notifier_wait() {
        _notifier->wake_wait();
    }

    // start the queue
    void run();

    // complete any buffers returned from the host
    void complete();

    // wait for the used ring to have at least @nr buffers
    future<> on_used(size_t nr);

    // Total number of descriptors in ring
    int size() { return _config.size; }

    template <typename Iterator>
    void post(Iterator begin, Iterator end);

    void flush_batch();

    semaphore& available_descriptors() { return _available_descriptors; }
private:
    // Let host know about interrupt delivery
    void disable_interrupts() {
        if (!_poll_mode && !_config.event_index) {
            _avail._shared->_flags.store(VRING_AVAIL_F_NO_INTERRUPT, std::memory_order_relaxed);
        }
    }

    // Return "true" if there are pending buffers in the queue
    bool enable_interrupts() {
        if (_poll_mode) {
            return false;
        }
        auto tail = _used._tail;
        if (!_config.event_index) {
            _avail._shared->_flags.store(0, std::memory_order_relaxed);
        } else {
            _used_event->store(tail, std::memory_order_relaxed);
        }

        // We need to set the host notification flag then check if the queue is
        // empty. The order is important. Use memory fence to make sure other
        // cores see the same order.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Any pending buffers
        auto used_head = _used._shared->_idx.load(std::memory_order_relaxed);
        return used_head != tail;
    }

    bool interrupts_disabled() {
        return (_avail._shared->_flags.load(std::memory_order_relaxed) & VRING_AVAIL_F_NO_INTERRUPT) != 0;
    }

    bool notifications_disabled() {
        return (_used._shared->_flags.load(std::memory_order_relaxed) & VRING_USED_F_NO_NOTIFY) != 0;
    }

    void kick() {
        bool need_kick = true;
        // Make sure we see the fresh _idx value writen before kick.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (_config.event_index) {
            uint16_t avail_idx = _avail._shared->_idx.load(std::memory_order_relaxed);
            uint16_t avail_event = _avail_event->load(std::memory_order_relaxed);
            need_kick = (uint16_t)(avail_idx - avail_event - 1) < _avail._avail_added_since_kick;
        } else {
            if (notifications_disabled())
                return;
        }
        if (need_kick || (_avail._avail_added_since_kick >= (uint16_t)(~0) / 2)) {
            _notifier->notify();
            _avail._avail_added_since_kick = 0;
        }
    }

    void do_complete();
    size_t mask() { return size() - 1; }
    size_t masked(size_t idx) { return idx & mask(); }
    size_t available();
    unsigned allocate_desc();
    void setup();
};

vring::avail::avail(config conf)
    : _shared(reinterpret_cast<avail_layout*>(conf.avail)) {
}

vring::used::used(config conf)
    : _shared(reinterpret_cast<used_layout*>(conf.used)) {
}

inline
unsigned vring::allocate_desc() {
    assert(_free_head != -1);
    auto desc = _free_head;
    if (desc == _free_last) {
        _free_last = _free_head = -1;
    } else {
        _free_head = _descs[desc]._next;
    }
    return desc;
}

vring::vring(config conf, bool poll_mode)
    : _config(conf)
    , _completions(new promise<size_t>[_config.size])
    , _descs(reinterpret_cast<desc*>(conf.descs))
    , _avail(conf)
    , _used(conf)
    , _avail_event(reinterpret_cast<std::atomic<uint16_t>*>(&_used._shared->_used_elements[conf.size]))
    , _used_event(reinterpret_cast<std::atomic<uint16_t>*>(&_avail._shared->_ring[conf.size]))
    , _poll_mode(poll_mode)
{
    setup();
}

void vring::setup() {
    for (unsigned i = 0; i < _config.size; ++i) {
        _descs[i]._next = i + 1;
    }
    _free_head = 0;
    _free_last = _config.size - 1;
    _available_descriptors.signal(_config.size);
}

void vring::run() {
    if (!_poll_mode) {
        complete();
    } else {
        _poller = reactor::poller([this] {
            flush_batch();
            do_complete();
            return true;
        });
    }
}

void vring::flush_batch() {
    if (_batch.empty()) {
        return;
    }
    for (auto desc_head : _batch) {
        _avail._shared->_ring[masked(_avail._head++)] = desc_head;
    }
    _batch.clear();
    _avail._shared->_idx.store(_avail._head, std::memory_order_release);
    kick();
}

template <typename Iterator>
void vring::post(Iterator begin, Iterator end) {
    // Note: buffer_chain here is any container of buffer, not
    //       necessarily vector<buffer>.
    //       buffer_chain should also include a promise<size_t> completed
    //       member, signifying the action to take when the request
    //       completes.
    using buffer_chain = decltype(*begin);
    std::for_each(begin, end, [this] (buffer_chain bc) {
        desc pseudo_head = {};
        desc* prev = &pseudo_head;
        for (auto i = bc.begin(); i != bc.end(); ++i) {
            unsigned desc_idx = allocate_desc();
            prev->_flags.has_next = true;
            prev->_next = desc_idx;
            desc &d = _descs[desc_idx];
            d._flags = {};
            auto b = *i;
            d._flags.writeable = b.writeable;
            d._paddr = b.addr;
            d._len = b.len;
            prev = &d;
        }
        auto desc_head = pseudo_head._next;
        _completions[desc_head] = std::move(bc.completed);
        if (!_poll_mode) {
            _avail._shared->_ring[masked(_avail._head++)] = desc_head;
        } else {
            _batch.push_back(desc_head);
        }
        _avail._avail_added_since_kick++;
    });
    if (!_poll_mode) {
        _avail._shared->_idx.store(_avail._head, std::memory_order_release);
        kick();
        do_complete();
    } else if (_batch.size() >= 16) {
        flush_batch();
    }
}

void vring::do_complete() {
    do {
        disable_interrupts();
        auto used_head = _used._shared->_idx.load(std::memory_order_acquire);
        while (used_head != _used._tail) {
            auto ue = _used._shared->_used_elements[masked(_used._tail++)];
            _completions[ue._id].set_value(ue._len);
            auto id = ue._id;
            if (_free_last != -1) {
                _descs[_free_last]._next = id;
            } else {
                _free_head = id;
            }
            unsigned count = 0;
            while (true) {
                auto& d = _descs[id];
                count++;
                if (!d._flags.has_next) {
                    break;
                }
                id = d._next;
            }
            _free_last = id;
        }
    } while (enable_interrupts());
}

void vring::complete() {
    do_complete();
    _notifier->wait().then([this] {
        complete();
    });
}

class qp : public net::qp {
protected:
    struct net_hdr {
        uint8_t needs_csum : 1;
        uint8_t flags_reserved : 7;
        enum { gso_none = 0, gso_tcpv4 = 1, gso_udp = 3, gso_tcpv6 = 4, gso_ecn = 0x80 };
        uint8_t gso_type;
        uint16_t hdr_len;
        uint16_t gso_size;
        uint16_t csum_start;
        uint16_t csum_offset;
    };
    struct net_hdr_mrg : net_hdr {
        uint16_t num_buffers;
    };
    class txq {
        qp& _dev;
        vring _ring;
    public:
        txq(qp& dev, vring::config config, bool poll_mode);
        void set_notifier(std::unique_ptr<notifier> notifier) {
            _ring.set_notifier(std::move(notifier));
        }
        const vring::config& getconfig() {
            return _ring.getconfig();
        }
        void wake_notifier_wait() {
            _ring.wake_notifier_wait();
        }
        void run() { _ring.run(); }
        future<> post(packet p);
    };
    class rxq  {
        qp& _dev;
        vring _ring;
        unsigned _remaining_buffers = 0;
        std::vector<fragment> _fragments;
        std::vector<std::unique_ptr<char[], free_deleter>> _deleters;
    public:
        rxq(qp& _if, vring::config config, bool poll_mode);
        void set_notifier(std::unique_ptr<notifier> notifier) {
            _ring.set_notifier(std::move(notifier));
        }
        const vring::config& getconfig() {
            return _ring.getconfig();
        }
        void run() {
            keep_doing([this] { return prepare_buffers(); });
            _ring.run();
        }
        void wake_notifier_wait() {
            _ring.wake_notifier_wait();
        }
    private:
        future<> prepare_buffers();
    };
protected:
    device* _dev;
    size_t _header_len;
    std::unique_ptr<char[], free_deleter> _txq_storage;
    std::unique_ptr<char[], free_deleter> _rxq_storage;
    txq _txq;
    rxq _rxq;
protected:
    vring::config txq_config(size_t txq_ring_size);
    vring::config rxq_config(size_t rxq_ring_size);
    void common_config(vring::config& r);
    size_t vring_storage_size(size_t ring_size);
public:
    explicit qp(device* dev, size_t rx_ring_size, size_t tx_ring_size, bool poll_mode);
    virtual future<> send(packet p) override;
    virtual void rx_start() override;
    virtual phys virt_to_phys(void* p) {
        return reinterpret_cast<uintptr_t>(p);
    }
};

qp::txq::txq(qp& dev, vring::config config, bool poll_mode)
    : _dev(dev), _ring(config, poll_mode) {
}

future<>
qp::txq::post(packet p) {
    net_hdr_mrg vhdr = {};

    // Handle TCP checksum offload
    auto oi = p.offload_info();
    if (_dev._dev->hw_features().tx_csum_l4_offload) {
        auto eth_hdr_len = sizeof(eth_hdr);
        auto ip_hdr_len = oi.ip_hdr_len;
        auto mtu = _dev._dev->hw_features().mtu;
        if (oi.protocol == ip_protocol_num::tcp) {
            auto tcp_hdr_len = oi.tcp_hdr_len;
            if (oi.needs_csum) {
                vhdr.needs_csum = 1;
                vhdr.csum_start = eth_hdr_len + ip_hdr_len;
                // TCP checksum filed's offset within the TCP header is 16 bytes
                vhdr.csum_offset = 16;
            }
            if (_dev._dev->hw_features().tx_tso && p.len() > mtu + eth_hdr_len) {
                // IPv4 TCP TSO
                vhdr.gso_type = net_hdr::gso_tcpv4;
                // Sum of Ethernet, IP and TCP header size
                vhdr.hdr_len = eth_hdr_len + ip_hdr_len + tcp_hdr_len;
                // Maximum segment size of packet after the offload
                vhdr.gso_size = mtu - ip_hdr_len - tcp_hdr_len;
            }
        } else if (oi.protocol == ip_protocol_num::udp) {
            auto udp_hdr_len = oi.udp_hdr_len;
            if (oi.needs_csum) {
                vhdr.needs_csum = 1;
                vhdr.csum_start = eth_hdr_len + ip_hdr_len;
                // UDP checksum filed's offset within the UDP header is 6 bytes
                vhdr.csum_offset = 6;
            }
            if (_dev._dev->hw_features().tx_ufo && p.len() > mtu + eth_hdr_len) {
                vhdr.gso_type = net_hdr::gso_udp;
                vhdr.hdr_len = eth_hdr_len + ip_hdr_len + udp_hdr_len;
                vhdr.gso_size = mtu - ip_hdr_len - udp_hdr_len;
            }
        }
    }

    // prepend virtio-net header
    packet q = packet(fragment{reinterpret_cast<char*>(&vhdr), _dev._header_len},
            std::move(p));

    auto nr_frags = q.nr_frags();
    return _ring.available_descriptors().wait(nr_frags).then([this, nr_frags, p = std::move(q)] () mutable {
        static auto fragment_to_buffer = [this] (fragment f) {
            vring::buffer b;
            b.addr = _dev.virt_to_phys(f.base);
            b.len = f.size;
            b.writeable = false;
            return b;
        };
        struct packet_as_buffer_chain {
            fragment* start;
            fragment* finish;
            promise<size_t> completed;
            auto begin() {
                return make_transform_iterator(start, fragment_to_buffer);
            }
            auto end() {
                return make_transform_iterator(finish, fragment_to_buffer);
            }
        } vbc[1] { { p.fragments().begin(), p.fragments().end() } };
        // schedule packet destruction
        vbc[0].completed.get_future().then([this, nr_frags, p = std::move(p)] (size_t) {
            _ring.available_descriptors().signal(nr_frags);
        });
        _ring.post(std::begin(vbc), std::end(vbc));
    });
}

qp::rxq::rxq(qp& dev, vring::config config, bool poll_mode)
    : _dev(dev), _ring(config, poll_mode) {
}

future<>
qp::rxq::prepare_buffers() {
    auto& available = _ring.available_descriptors();
    return available.wait(1).then([this, &available] {
        unsigned count = 1;
        auto opportunistic = available.current();
        if (available.try_wait(opportunistic)) {
            count += opportunistic;
        }
        auto make_buffer_chain = [this] {
            struct single_buffer_and_completion : std::array<vring::buffer, 1> {
                promise<size_t> completed;
            } bc;
            std::unique_ptr<char[], free_deleter> buf(reinterpret_cast<char*>(malloc(4096)));
            vring::buffer& b = bc[0];
            b.addr = _dev.virt_to_phys(buf.get());
            b.len = 4096;
            b.writeable = true;
            bc.completed.get_future().then([this, buf = std::move(buf)] (size_t len) mutable {
                auto frag_buf = buf.get();
                auto frag_len = len;
                // First buffer
                if (_remaining_buffers == 0) {
                    auto hdr = reinterpret_cast<net_hdr_mrg*>(frag_buf);
                    assert(hdr->num_buffers >= 1);
                    // TODO: special-case for num_buffers == 1
                    _remaining_buffers = hdr->num_buffers;
                    frag_buf += _dev._header_len;
                    frag_len -= _dev._header_len;
                    _fragments.clear();
                    _deleters.clear();
                };

                // Append current buffer
                _fragments.emplace_back(fragment{frag_buf, frag_len});
                _deleters.push_back(std::move(buf));
                _remaining_buffers--;

                // Last buffer
                if (_remaining_buffers == 0) {
                    deleter del;
                    if (_deleters.size() == 1) {
                        del = make_free_deleter(_deleters[0].release());
                        _deleters.clear();
                    } else {
                        del = make_deleter(deleter(), [deleters = std::move(_deleters)] {});
                    }
                    packet p(_fragments.begin(), _fragments.end(), std::move(del));
                    _dev._dev->l2receive(std::move(p));
                    _ring.available_descriptors().signal(_fragments.size());
                }
            });
            return bc;
        };
        auto start = make_function_input_iterator(make_buffer_chain, 0U);
        auto finish = make_function_input_iterator(make_buffer_chain, count);
        _ring.post(start, finish);
    });
}

// Allocate and zero-initialize a buffer which is page-aligned and can be
// used for virt_to_phys (i.e., physically contiguous).
static std::unique_ptr<char[], free_deleter> virtio_buffer(size_t size) {
    void* ret;
    auto r = posix_memalign(&ret, 4096, size);
    assert(r == 0);
    bzero(ret, size);
    return std::unique_ptr<char[], free_deleter>(reinterpret_cast<char*>(ret));
}

qp::qp(device* dev, size_t rx_ring_size, size_t tx_ring_size, bool poll_mode)
    : _dev(dev)
    , _txq_storage(virtio_buffer(vring_storage_size(tx_ring_size)))
    , _rxq_storage(virtio_buffer(vring_storage_size(rx_ring_size)))
    , _txq(*this, txq_config(tx_ring_size), poll_mode)
    , _rxq(*this, rxq_config(rx_ring_size), poll_mode) {
}

size_t qp::vring_storage_size(size_t ring_size) {
    // overestimate, but not by much.
    return 3 * 4096 + ring_size * (16 + 2 + 8);
}

void qp::common_config(vring::config& r) {
    r.avail = r.descs + 16 * r.size;
    r.used = align_up(r.avail + 2 * r.size + 6, 4096);
    r.event_index = (_dev->features() & VIRTIO_RING_F_EVENT_IDX) != 0;
    r.indirect = false;
}

vring::config qp::txq_config(size_t tx_ring_size) {
    vring::config r;
    r.size = tx_ring_size;
    r.descs = _txq_storage.get();
    r.mergable_buffers = false;
    common_config(r);
    return r;
}

vring::config qp::rxq_config(size_t rx_ring_size) {
    vring::config r;
    r.size = rx_ring_size;
    r.descs = _rxq_storage.get();
    r.mergable_buffers = true;
    common_config(r);
    return r;
}

void
qp::rx_start() {
    _rxq.run();
}

future<>
qp::send(packet p) {
    return _txq.post(std::move(p));
}

class qp_vhost : public qp {
private:
    // The vhost file descriptor needs to remain open throughout the life of
    // this driver, as as soon as we close it, vhost stops servicing us.
    file_desc _vhost_fd;
public:
    qp_vhost(device* dev, boost::program_options::variables_map opts);
};

static size_t config_ring_size(boost::program_options::variables_map &opts) {
    if (opts.count("event-index")) {
        return opts["virtio-ring-size"].as<unsigned>();
    } else {
        return 256;
    }
}

qp_vhost::qp_vhost(device *dev, boost::program_options::variables_map opts)
    : qp(dev, config_ring_size(opts), config_ring_size(opts), opts["virtio-poll-mode"].as<bool>())
    , _vhost_fd(file_desc::open("/dev/vhost-net", O_RDWR))
{
    auto tap_device = opts["tap-device"].as<std::string>();
    int64_t vhost_supported_features;
    _vhost_fd.ioctl(VHOST_GET_FEATURES, vhost_supported_features);
    vhost_supported_features &= _dev->features();
    _vhost_fd.ioctl(VHOST_SET_FEATURES, vhost_supported_features);
    if (vhost_supported_features & VIRTIO_NET_F_MRG_RXBUF) {
        _header_len = sizeof(net_hdr_mrg);
    } else {
        _header_len = sizeof(net_hdr);
    }

    // Open and set up the tap device, which we'll tell vhost to use.
    // Note that the tap_fd we open here will be closed at the end of
    // this function. It appears that this is fine - i.e., after we pass
    // this fd to VHOST_NET_SET_BACKEND, the Linux kernel keeps the reference
    // to it and it's fine to close the file descriptor.
    file_desc tap_fd(file_desc::open("/dev/net/tun", O_RDWR | O_NONBLOCK));
    assert(tap_device.size() + 1 <= IFNAMSIZ);
    ifreq ifr = {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_ONE_QUEUE | IFF_VNET_HDR;
    strcpy(ifr.ifr_ifrn.ifrn_name, tap_device.c_str());
    tap_fd.ioctl(TUNSETIFF, ifr);
    unsigned int offload = 0;
    auto hw_features = _dev->hw_features();
    if (hw_features.tx_csum_l4_offload && hw_features.rx_csum_offload) {
        offload = TUN_F_CSUM;
        if (hw_features.tx_tso) {
            offload |= TUN_F_TSO4;
        }
        if (hw_features.tx_ufo) {
            offload |= TUN_F_UFO;
        }
    }
    tap_fd.ioctl(TUNSETOFFLOAD, offload);
    tap_fd.ioctl(TUNSETVNETHDRSZ, _header_len);

    // Additional vhost setup:
    _vhost_fd.ioctl(VHOST_SET_OWNER);
    auto mem_table = make_struct_with_vla(&vhost_memory::regions, 1);
    mem_table->nregions = 1;
    auto& region = mem_table->regions[0];
    region.guest_phys_addr = 0;
    region.memory_size = (size_t(1) << 47) - 4096;
    region.userspace_addr = 0;
    region.flags_padding = 0;
    _vhost_fd.ioctl(VHOST_SET_MEM_TABLE, *mem_table);
    vhost_vring_state vvs0 = { 0, _rxq.getconfig().size };
    _vhost_fd.ioctl(VHOST_SET_VRING_NUM, vvs0);
    vhost_vring_state vvs1 = { 1, _txq.getconfig().size };
    _vhost_fd.ioctl(VHOST_SET_VRING_NUM, vvs1);
    auto tov = [](char* x) { return reinterpret_cast<uintptr_t>(x); };

    _vhost_fd.ioctl(VHOST_SET_VRING_ADDR, vhost_vring_addr{
        0, 0, tov(_rxq.getconfig().descs), tov(_rxq.getconfig().used),
        tov(_rxq.getconfig().avail), 0
    });
    _vhost_fd.ioctl(VHOST_SET_VRING_ADDR, vhost_vring_addr{
        1, 0, tov(_txq.getconfig().descs), tov(_txq.getconfig().used),
        tov(_txq.getconfig().avail), 0
    });

    readable_eventfd _txq_notify;
    writeable_eventfd _txq_kick;
    readable_eventfd _rxq_notify;
    writeable_eventfd _rxq_kick;
    _vhost_fd.ioctl(VHOST_SET_VRING_KICK, vhost_vring_file{0, _rxq_kick.get_read_fd()});
    _vhost_fd.ioctl(VHOST_SET_VRING_CALL, vhost_vring_file{0, _rxq_notify.get_write_fd()});
    _vhost_fd.ioctl(VHOST_SET_VRING_KICK, vhost_vring_file{1, _txq_kick.get_read_fd()});
    _vhost_fd.ioctl(VHOST_SET_VRING_CALL, vhost_vring_file{1, _txq_notify.get_write_fd()});
    _rxq.set_notifier(std::make_unique<notifier_vhost>(
            std::move(_rxq_notify), std::move(_rxq_kick)));
    _txq.set_notifier(std::make_unique<notifier_vhost>(
            std::move(_txq_notify), std::move(_txq_kick)));

    _vhost_fd.ioctl(VHOST_NET_SET_BACKEND, vhost_vring_file{0, tap_fd.get()});
    _vhost_fd.ioctl(VHOST_NET_SET_BACKEND, vhost_vring_file{1, tap_fd.get()});

    _txq.run();
}

#ifdef HAVE_OSV
class qp_osv : public qp {
private:
    ethernet_address _mac;
    osv::assigned_virtio &_virtio;
public:
    qp_osv(osv::assigned_virtio &virtio,
            boost::program_options::variables_map opts);
    virtual ethernet_address hw_address() override {
        return _mac;
    }
    virtual phys virt_to_phys(void* p) override {
        return osv::assigned_virtio::virt_to_phys(p);
    }
};

qp_osv::qp_osv(osv::assigned_virtio &virtio,
        boost::program_options::variables_map opts)
        : qp(opts, virtio.queue_size(0), virtio.queue_size(1), opts["virtio-poll-mode"].as<bool>())
        , _virtio(virtio)
{
    // Read the host's virtio supported feature bitmask, AND it with the
    // features we want to use, and tell the host of the result:
    uint32_t subset = _virtio.init_features(_dev->features());
    if (subset & VIRTIO_NET_F_MRG_RXBUF) {
        _header_len = sizeof(net_hdr_mrg);
    } else {
        _header_len = sizeof(net_hdr);
    }

    // TODO: save bits from "subset" in _hw_features?
//    bool _mergeable_bufs = subset & VIRTIO_NET_F_MRG_RXBUF;
//    bool _status = subset & VIRTIO_NET_F_STATUS;
//    bool _tso_ecn = subset & VIRTIO_NET_F_GUEST_ECN;
//    bool _host_tso_ecn = subset & VIRTIO_NET_F_HOST_ECN;
//    bool _csum = subset & VIRTIO_NET_F_CSUM;
//    bool _guest_csum = subset & VIRTIO_NET_F_GUEST_CSUM;
//    bool _guest_tso4 = subset & VIRTIO_NET_F_GUEST_TSO4;
//    bool _host_tso4 = subset & VIRTIO_NET_F_HOST_TSO4;
//    bool _guest_ufo = subset & VIRTIO_NET_F_GUEST_UFO;

    // Get the MAC address set by the host
    assert(subset & VIRTIO_NET_F_MAC);
    struct net_config {
        /* The config defining mac address (if VIRTIO_NET_F_MAC) */
        uint8_t mac[6];
        /* See VIRTIO_NET_F_STATUS and VIRTIO_NET_S_* */
        uint16_t status;
        /* Maximum number of each of transmit and receive queues;
         * see VIRTIO_NET_F_MQ and VIRTIO_NET_CTRL_MQ.
         * Legal values are between 1 and 0x8000
         */
        uint16_t max_virtqueue_pairs;
    } __attribute__((packed)) host_config;
    _virtio.conf_read(&host_config, sizeof(host_config));
    _mac = { host_config.mac[0], host_config.mac[1], host_config.mac[2],
             host_config.mac[3], host_config.mac[4], host_config.mac[5] };

    // Setup notifiers
    _rxq.set_notifier(std::make_unique<notifier_osv>(_virtio, 0));
    _txq.set_notifier(std::make_unique<notifier_osv>(_virtio, 1));


    // Tell the host where we put the rings (we already allocated them earlier)
    _virtio.set_queue_pfn(
            0, virt_to_phys(_rxq.getconfig().descs));
    _virtio.set_queue_pfn(
            1, virt_to_phys(_txq.getconfig().descs));

    _txq.run();

    // Set up interrupts
    // FIXME: in OSv, the first thing we do in the handler is to call
    // _rqx.disable_interrupts(). Here in seastar, we only do it much later
    // in the main engine. Probably needs to do it like in osv - in the beginning of the handler.
    _virtio.enable_interrupt(
            0, [&] { _rxq.wake_notifier_wait(); } );
    _virtio.enable_interrupt(
            1, [&] { _txq.wake_notifier_wait(); } );

    _virtio.set_driver_ok();
}
#endif

void device::init_local_queue(boost::program_options::variables_map opts) {
    std::unique_ptr<net::qp> ptr;

    if (engine.cpu_id() == 0) {
#ifdef HAVE_OSV
        if (osv::assigned_virtio::get && osv::assigned_virtio::get()) {
            std::cout << "In OSv and assigned host's virtio device\n";
            ptr = std::make_unique<qp_osv>(*osv::assigned_virtio::get(), opts);
        } else
#endif
        ptr = std::make_unique<qp_vhost>(this, opts);

        for (unsigned i = 0; i < smp::count; i++) {
            if (i != engine.cpu_id()) {
                ptr->add_proxy(i);
            }
        }
    } else {
        ptr = create_proxy_net_device(0, this);
    }
    set_local_queue(std::move(ptr));
}

}

boost::program_options::options_description
get_virtio_net_options_description()
{
    boost::program_options::options_description opts(
            "Virtio net options");
    opts.add_options()
        ("event-index",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable event-index feature (on / off)")
        ("csum-offload",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable checksum offload feature (on / off)")
        ("tso",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable TCP segment offload feature (on / off)")
        ("ufo",
                boost::program_options::value<std::string>()->default_value("on"),
                "Enable UDP fragmentation offload feature (on / off)")
        ("virtio-ring-size",
                boost::program_options::value<unsigned>()->default_value(256),
                "Virtio ring size (must be power-of-two)")
        ("virtio-poll-mode",
                boost::program_options::value<bool>()->default_value(false),
                "Poll virtio rings instead of using interrupts")
        ;
    return opts;
}

std::unique_ptr<net::device> create_virtio_net_device(boost::program_options::variables_map opts) {

    if (engine.cpu_id() == 0) {
        return std::make_unique<virtio::device>(opts);
    } else {
        return nullptr;
    }
}

// Locks the shared object in memory and forces on-load function resolution.
// Needed if the function passed to enable_interrupt() is run at interrupt
// time.
// TODO: Instead of doing this, _virtio.enable_interrupt() could take a
// pollable to wake instead of a function, then this won't be needed.
asm(".pushsection .note.osv-mlock, \"a\"; .long 0, 0, 0; .popsection");
