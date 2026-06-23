#ifndef HSCRCPY_NET_UTIL_H
#define HSCRCPY_NET_UTIL_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace hscrcpy {

// Enable TCP keepalive on a socket to detect silently dropped connections.
// This is critical for TCP devices where NAT/firewall can drop idle connections
// without sending RST/FIN.
// Parameters: 30s idle before first probe, 10s between probes, 3 failed probes = dead
inline bool set_tcp_keepalive(int sock) {
#ifdef _WIN32
    int keepalive = 1;
    if (setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char *>(&keepalive), sizeof(keepalive)) != 0)
        return false;
    DWORD idle_ms = 30000;
    if (setsockopt(static_cast<SOCKET>(sock), IPPROTO_TCP, TCP_KEEPIDLE,
                   reinterpret_cast<const char *>(&idle_ms), sizeof(idle_ms)) != 0)
        return false;
    DWORD interval_ms = 10000;
    if (setsockopt(static_cast<SOCKET>(sock), IPPROTO_TCP, TCP_KEEPINTVL,
                   reinterpret_cast<const char *>(&interval_ms), sizeof(interval_ms)) != 0)
        return false;
    DWORD count = 3;
    if (setsockopt(static_cast<SOCKET>(sock), IPPROTO_TCP, TCP_KEEPCNT,
                   reinterpret_cast<const char *>(&count), sizeof(count)) != 0)
        return false;
    return true;
#else
    int keepalive = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0)
        return false;
    int idle = 30;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0)
        return false;
    int interval = 10;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0)
        return false;
    int count = 3;
    if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0)
        return false;
    return true;
#endif
}

} // namespace hscrcpy

#endif
