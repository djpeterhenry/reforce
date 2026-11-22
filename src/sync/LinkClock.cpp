#include "sync/LinkClock.hpp"

#include <ableton/Link.hpp>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace reforce::sync {

namespace {

constexpr uint16_t kDiscoveryPort = 20808;  // Link discovery (ableton/discovery/IpInterface.hpp)
constexpr const char* kGroupV4 = "224.76.78.75";
constexpr const char* kGroupV6 = "ff12::8080";

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A Link v1 discovery alive/response message: "_asdp_v\1", type, ttl, group (2), node id (8).
bool isPeerAnnouncement(const unsigned char* p, ssize_t n) {
    static const unsigned char kHeader[8] = {'_', 'a', 's', 'd', 'p', '_', 'v', 1};
    return n >= 20 && std::memcmp(p, kHeader, 8) == 0 && (p[8] == 1 || p[8] == 2);
}

// Bound to the discovery port (any address) like Link's own multicast sockets. It
// must set SO_REUSEPORT as well: on macOS SO_REUSEADDR alone does not let two
// sockets share a wildcard UDP port, so this listener would lock Link apps (and
// reforce's own Link once it joins) out of discovery.
int openListener(int family) {
    const int fd = ::socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof yes);
    int rc;
    if (family == AF_INET) {
        sockaddr_in a{};
        a.sin_len = sizeof a;
        a.sin_family = AF_INET;
        a.sin_port = htons(kDiscoveryPort);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof yes);
        sockaddr_in6 a{};
        a.sin6_len = sizeof a;
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(kDiscoveryPort);
        a.sin6_addr = in6addr_any;
        rc = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Joins the discovery groups on every up, multicast-capable interface (loopback
// included: Link apps on the same Mac announce there). Repeats are harmless
// (already-joined groups fail with EADDRINUSE), so it is re-run to pick up
// interfaces that come up later.
void joinGroups(int fd4, int fd6) {
    ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0) return;
    for (const ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & (IFF_MULTICAST | IFF_LOOPBACK))) continue;
        if (i->ifa_addr->sa_family == AF_INET && fd4 >= 0) {
            ip_mreq m{};
            ::inet_pton(AF_INET, kGroupV4, &m.imr_multiaddr);
            m.imr_interface = reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr;
            ::setsockopt(fd4, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);
        } else if (i->ifa_addr->sa_family == AF_INET6 && fd6 >= 0) {
            const auto* a6 = reinterpret_cast<const sockaddr_in6*>(i->ifa_addr);
            if (!IN6_IS_ADDR_LINKLOCAL(&a6->sin6_addr)) continue;
            ipv6_mreq m{};
            ::inet_pton(AF_INET6, kGroupV6, &m.ipv6mr_multiaddr);
            m.ipv6mr_interface = ::if_nametoindex(i->ifa_name);
            ::setsockopt(fd6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, sizeof m);
        }
    }
    ::freeifaddrs(ifs);
}

}  // namespace

struct LinkClock::Impl {
    Impl(double bpm, bool follow) : link(bpm), follow(follow) {}
    ~Impl() { stopFollowing(); }

    ableton::Link link;
    const bool follow;
    std::atomic<bool> wanted{false};
    std::atomic<bool> heard{false};  // follow mode: another Link app announced recently
    std::thread watcher;
    std::atomic<bool> stop{false};

    void startFollowing() {
        if (watcher.joinable()) return;
        stop = false;
        watcher = std::thread([this] { watch(); });
    }

    void stopFollowing() {
        stop = true;
        if (watcher.joinable()) watcher.join();
        heard = false;
        link.enable(false);
    }

    void watch() {
        const int fd4 = openListener(AF_INET), fd6 = openListener(AF_INET6);
        if (fd4 < 0 && fd6 < 0) {
            // Without a listener reforce cannot tell when to join: lead instead of never linking.
            std::fprintf(stderr, "[reforce] Link: cannot listen on port %u; joining at once (may set the tempo)\n",
                         unsigned(kDiscoveryPort));
            link.enable(true);
            return;
        }
        LinkFollower f;
        double rescanAt = 0.0, quietUntil = 0.0;
        unsigned char buf[512];
        while (!stop) {
            double now = nowSeconds();
            if (now >= rescanAt) {
                joinGroups(fd4, fd6);
                rescanAt = now + 10.0;
            }
            pollfd p[2] = {{fd4, POLLIN, 0}, {fd6, POLLIN, 0}};
            if (::poll(p, 2, 100) > 0) {
                now = nowSeconds();
                for (const pollfd& q : p) {
                    if (q.fd < 0 || !(q.revents & POLLIN)) continue;
                    ssize_t n;
                    while ((n = ::recv(q.fd, buf, sizeof buf, MSG_DONTWAIT)) > 0)
                        if (isPeerAnnouncement(buf, n) && now >= quietUntil) f.onAnnouncement(now);
                }
            }
            now = nowSeconds();
            const int peers = link.isEnabled() ? int(link.numPeers()) : 0;
            switch (f.update(now, peers)) {
            case +1:
                std::fprintf(stderr, "[reforce] Link: another Link app found; joining its session\n");
                link.enable(true);
                break;
            case -1:
                std::fprintf(stderr, "[reforce] Link: no peers; leaving the session and listening again\n");
                link.enable(false);
                quietUntil = now + 1.0;  // our own last announcements may still be in flight
                break;
            default: break;
            }
            heard = f.joined() || f.heard(now);
        }
        if (fd4 >= 0) ::close(fd4);
        if (fd6 >= 0) ::close(fd6);
        link.enable(false);
    }
};

LinkClock::LinkClock(double quantum, double initialBpm, bool follow)
    : impl_(std::make_unique<Impl>(initialBpm, follow)), quantum_(quantum > 0.0 ? quantum : 4.0) {}

LinkClock::~LinkClock() {
    if (impl_) {
        impl_->stopFollowing();
        impl_->link.enable(false);
    }
}

void LinkClock::setEnabled(bool on) {
    impl_->wanted = on;
    if (impl_->follow) {
        if (on) impl_->startFollowing();
        else impl_->stopFollowing();
    } else {
        impl_->link.enable(on);
    }
}

bool LinkClock::enabled() const { return impl_->wanted; }

LinkSnapshot LinkClock::sample(double offsetSeconds) const {
    LinkSnapshot s;
    s.quantum = quantum_;
    s.enabled = impl_->wanted;
    if (!s.enabled) return s;
    const bool linkOn = impl_->link.isEnabled();
    s.peers = linkOn ? int(impl_->link.numPeers()) : 0;
    s.on = s.peers > 0;
    if (impl_->follow && !s.on) {
        s.joining = impl_->heard;
        s.listening = !s.joining;
    }
    if (!s.on) return s;
    const auto state = impl_->link.captureAppSessionState();
    const auto at = impl_->link.clock().micros() +
                    std::chrono::microseconds(static_cast<long long>(std::llround(offsetSeconds * 1e6)));
    s.bpm = state.tempo();
    s.beat = state.beatAtTime(at, quantum_);
    s.phase = state.phaseAtTime(at, quantum_);
    return s;
}

void LinkClock::proposeTempo(double bpm) {
    auto state = impl_->link.captureAppSessionState();
    state.setTempo(bpm, impl_->link.clock().micros());
    impl_->link.commitAppSessionState(state);
}

}  // namespace reforce::sync
