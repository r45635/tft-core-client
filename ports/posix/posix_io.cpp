#include "posix_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace tftcore::posix {

bool TcpStream::connect(const char *host, uint16_t port, uint32_t timeoutMs) {
    close();
    char portText[8];
    snprintf(portText, sizeof portText, "%u", (unsigned)port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host, portText, &hints, &res) != 0 || !res) return false;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return false;
    }
    timeval tv{(time_t)(timeoutMs / 1000), (suseconds_t)((timeoutMs % 1000) * 1000)};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    bool ok = ::connect(fd, res->ai_addr, res->ai_addrlen) == 0;
    freeaddrinfo(res);
    if (!ok) {
        ::close(fd);
        return false;
    }
    // Reads must not block the state machine; a short timeout keeps the handshake loop cheap.
    timeval rtv{0, 20000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof rtv);
    fd_ = fd;
    return true;
}

int TcpStream::read(uint8_t *buf, size_t n) {
    if (fd_ < 0) return -1;
    ssize_t got = ::recv(fd_, buf, n, 0);
    if (got > 0) return (int)got;
    if (got == 0) return -1;   // the peer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int TcpStream::write(const uint8_t *buf, size_t n) {
    if (fd_ < 0) return -1;
    size_t sent = 0;
    while (sent < n) {
        ssize_t got = ::send(fd_, buf + sent, n - sent, 0);
        if (got <= 0) {
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        sent += (size_t)got;
    }
    return (int)sent;
}

void TcpStream::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Udp::open() {
    close();
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    unsigned char ttl = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (sockaddr *)&addr, sizeof addr) != 0) {
        ::close(fd);
        return false;
    }
    timeval tv{0, 50000};   // 50 ms: recvFrom waits a little instead of spinning
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    fd_ = fd;
    return true;
}

bool Udp::sendTo(const char *ip, uint16_t port, const uint8_t *buf, size_t n) {
    if (fd_ < 0) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) return false;
    return ::sendto(fd_, buf, n, 0, (sockaddr *)&to, sizeof to) == (ssize_t)n;
}

bool Udp::sendBroadcast(uint16_t port, const uint8_t *buf, size_t n) {
    if (fd_ < 0) return false;
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    bool any = false;
    for (ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_BROADCAST) || !(i->ifa_flags & IFF_UP)) continue;
        if (!i->ifa_broadaddr) continue;
        sockaddr_in to = *(sockaddr_in *)i->ifa_broadaddr;
        to.sin_port = htons(port);
        if (::sendto(fd_, buf, n, 0, (sockaddr *)&to, sizeof to) == (ssize_t)n) any = true;
    }
    freeifaddrs(list);
    return any;
}

int Udp::recvFrom(uint8_t *buf, size_t n, char *fromIp, size_t fromIpLen) {
    if (fd_ < 0) return -1;
    sockaddr_in from{};
    socklen_t fromLen = sizeof from;
    ssize_t got = ::recvfrom(fd_, buf, n, 0, (sockaddr *)&from, &fromLen);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (fromIp && fromIpLen) {
        fromIp[0] = 0;
        inet_ntop(AF_INET, &from.sin_addr, fromIp, (socklen_t)fromIpLen);
    }
    return (int)got;
}

// The first routable IPv4 address of an interface that is up: on a machine with a Thunderbolt
// bridge or a VPN this is the one a local server is reachable on.
bool Udp::localAddress(char *out, size_t outLen) {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return false;
    bool found = false;
    for (ifaddrs *i = list; i && !found; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t ip = ntohl(((sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if ((ip >> 16) == 0xa9fe) continue;   // 169.254/16, a link-local address goes nowhere
        if (!inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)outLen)) continue;
        found = true;
    }
    freeifaddrs(list);
    return found;
}

void Udp::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

uint32_t SteadyClock::nowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

}  // namespace tftcore::posix
