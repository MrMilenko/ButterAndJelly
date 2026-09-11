// SPDX-License-Identifier: GPL-2.0-or-later

// discovery.cpp: Jellyfin's UDP auto-discovery.
//
// The server listens on UDP 7359 and answers the literal string
// "who is JellyfinServer?" with a small JSON object naming itself. This is
// the primary path; manual entry is the fallback.

#include "core/jellyfin.h"

#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/thread.h"
#include "core/text.h"

#include <cerrno>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_XBOX)
  // winsockx.h maps recvfrom, sendto and the rest onto XSocket*, so the code
  // below is unchanged. Before the _WIN32 branch: the toolchain defines that
  // too, and there is no ws2tcpip.h.
  #include <xtl.h>
  #include <winsockx.h>
  // winsockx.h has the calls but not this constant, which belongs to the
  // address format rather than to any stack: "255.255.255.255" and a NUL.
  #ifndef INET_ADDRSTRLEN
    #define INET_ADDRSTRLEN 16
  #endif
  #define BJ_NEEDS_INET_NTOP 1
  // See the note in http_sockets.cpp: bypassing security at startup is not
  // enough, every socket has to be marked unencrypted as well or the stack
  // silently encrypts what it sends and drops what comes back.
  #define BJ_SO_UNENCRYPTED 0x5801
  using socklen_t = int;
  #define CLOSE_SOCKET closesocket
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#elif defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socklen_t = int;
  #define CLOSE_SOCKET closesocket
  using socket_t = SOCKET;
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <fcntl.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  #define CLOSE_SOCKET close
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
#endif

namespace {

// errno says nothing about a socket on Winsock-shaped stacks, where the error
// lives in WSAGetLastError instead. Everything below reports through this.
int SocketError()
{
#if defined(_XBOX) || defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Non-blocking, spelled three ways. The Wii U has SO_NONBLOCK, Winsock has
// ioctlsocket, and everything else has fcntl.
bool SetNonBlocking(socket_t sock)
{
#if defined(_XBOX) || defined(_WIN32)
    unsigned long on = 1;
    return ioctlsocket(sock, FIONBIO, &on) == 0;
#elif defined(__WIIU__)
    int on = 1;
    return setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, (const char*)&on, sizeof(on)) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    return flags != -1 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

// select()'s first argument is the highest descriptor plus one on Berkeley
// stacks and ignored on Winsock ones, where a socket is a handle rather than a
// small integer, so "sock + 1" is a nonsense number to hand it.
int SelectWidth(socket_t sock)
{
#if defined(_XBOX) || defined(_WIN32)
    (void)sock;
    return 0;
#else
    return (int)sock + 1;
#endif
}

#ifdef BJ_NEEDS_INET_NTOP
// The XDK ships inet_addr and nothing going the other way: no inet_ntop, not
// even inet_ntoa. Printing four bytes is not worth a dependency.
const char* inet_ntop(int family, const void* address, char* out, size_t outSize)
{
    if (family != AF_INET || !address || !out) return nullptr;
    const unsigned char* b = (const unsigned char*)address;
    const int written = std::snprintf(out, outSize, "%u.%u.%u.%u",
                                      b[0], b[1], b[2], b[3]);
    return (written > 0 && (size_t)written < outSize) ? out : nullptr;
}
#endif

constexpr int   kDiscoveryPort  = 7359;
constexpr char  kProbeMessage[] = "who is JellyfinServer?";

}  // namespace

std::vector<JfServer> JellyfinClient::Discover(int waitMs)
{
    std::vector<JfServer> found;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return found;
#endif

    socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) {
        LOGF("[discovery] socket() failed, errno=%d", SocketError());
        return found;
    }

    // Not fatal if the stack refuses it: the subnet-directed address below
    // often still gets through, and bailing here means no discovery at all.
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   (const char*)&broadcast, sizeof(broadcast)) != 0) {
        LOGF("[discovery] SO_BROADCAST rejected, errno=%d (continuing)", SocketError());
    }

#ifdef BJ_SO_UNENCRYPTED
    {
        BOOL unencrypted = TRUE;
        if (setsockopt(sock, SOL_SOCKET, BJ_SO_UNENCRYPTED,
                       (const char*)&unencrypted, sizeof(unencrypted)) != 0) {
            LOGF("[discovery] could not mark the socket unencrypted, errno=%d",
                 SocketError());
        }
    }
#endif

    // Bind explicitly. Relying on sendto's implicit bind is enough on some
    // stacks but leaves nothing listening for the reply on others.
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = 0;
    if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0) {
        LOGF("[discovery] bind() failed, errno=%d (continuing)", SocketError());
    }

    // Probe the global broadcast address and, when we know our own address,
    // the subnet-directed one. A /24 is an assumption, but it holds for the
    // home networks this runs on and costs one extra packet when it does not.
    uint32_t targets[2] = { INADDR_BROADCAST, 0 };
    int targetCount = 1;

    const uint32_t localIp = Platform::LocalIPv4();
    if (localIp != 0) {
        targets[1] = (localIp & 0xFFFFFF00u) | 0xFFu;
        targetCount = 2;
        LOGF("[discovery] local %u.%u.%u.%u, also probing %u.%u.%u.255",
             (localIp >> 24) & 0xFF, (localIp >> 16) & 0xFF,
             (localIp >> 8) & 0xFF, localIp & 0xFF,
             (localIp >> 24) & 0xFF, (localIp >> 16) & 0xFF, (localIp >> 8) & 0xFF);
    } else {
        LOGF("[discovery] local address unknown, global broadcast only");
    }

    // Two rounds: the first packet is often lost while wifi finishes waking
    // up, and duplicate replies are deduplicated on server id below.
    for (int round = 0; round < 2; ++round) {
        for (int t = 0; t < targetCount; ++t) {
            sockaddr_in dest{};
            dest.sin_family      = AF_INET;
            dest.sin_port        = htons(kDiscoveryPort);
            dest.sin_addr.s_addr = htonl(targets[t]);

            const long sent = sendto(sock, kProbeMessage, sizeof(kProbeMessage) - 1,
                                     0, (sockaddr*)&dest, sizeof(dest));
            LOGF("[discovery] probe %u.%u.%u.%u -> sendto returned %ld (errno=%d)",
                 (targets[t] >> 24) & 0xFF, (targets[t] >> 16) & 0xFF,
                 (targets[t] >> 8) & 0xFF, targets[t] & 0xFF, sent,
                 sent < 0 ? SocketError() : 0);
        }
    }

    const int64_t  windowMs  = waitMs > 0 ? waitMs : 1500;
    const uint64_t startedAt = Platform::NowMs();
    const uint64_t deadline  = startedAt + (uint64_t)windowMs;

    while (Platform::NowMs() < deadline) {
        const int64_t remainingMs = (int64_t)(deadline - Platform::NowMs());

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);

        timeval tv{};
        tv.tv_sec  = (long)(remainingMs / 1000);
        tv.tv_usec = (long)((remainingMs % 1000) * 1000);

        const int ready = select(SelectWidth(sock), &readSet, nullptr, nullptr, &tv);
        if (ready < 0) {
            LOGF("[discovery] select() failed, errno=%d", SocketError());
            break;
        }
        if (ready == 0) {
            // If this lands far short of the window, select() is not honouring
            // its timeout and the whole listen phase never really happened.
            LOGF("[discovery] select() timed out after %lums of %lums",
                 (unsigned long)(Platform::NowMs() - startedAt),
                 (unsigned long)windowMs);
            break;
        }

        char buf[2048];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const long n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                (sockaddr*)&from, &fromLen);
        if (n <= 0) {
            LOGF("[discovery] recvfrom returned %ld, errno=%d", n, SocketError());
            break;
        }
        buf[n] = '\0';

        JsonDoc doc(std::string(buf, (size_t)n));
        if (!doc.valid()) {
            LOGF("[discovery] ignoring %ld bytes that are not JSON", n);
            continue;
        }

        JfServer srv;
        srv.id      = doc["Id"].asString();
        srv.name    = doc["Name"].asString();
        srv.address = doc["Address"].asString();

        // Older servers answer with no scheme, or no address at all; rebuild
        // a usable URL from the packet's source in that case.
        if (srv.address.empty()) {
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            if (ip[0]) srv.address = std::string("http://") + ip + ":8096";
        }
        if (!srv.address.empty() && srv.address.find("://") == std::string::npos) {
            srv.address = "http://" + srv.address;
        }
        while (!srv.address.empty() && srv.address.back() == '/') srv.address.pop_back();
        if (srv.name.empty()) srv.name = "Jellyfin";

        if (srv.address.empty()) continue;

        bool duplicate = false;
        for (const JfServer& existing : found) {
            if ((!srv.id.empty() && existing.id == srv.id) ||
                existing.address == srv.address) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            LOGF("[discovery] reply: %s at %s", srv.name.c_str(), srv.address.c_str());
            found.push_back(srv);
        }
    }

    // Fallback: retry the listen without select() at all, in case the stack
    // implements it poorly for sockets. Wii U has no SO_RCVTIMEO, so this
    // polls a non-blocking socket rather than using a receive timeout.
    if (found.empty()) {
        const bool nonBlocking = SetNonBlocking(sock);
        if (!nonBlocking) {
            LOGF("[discovery] could not set non-blocking, errno=%d - no retry",
                 SocketError());
        } else {
            for (int t = 0; t < targetCount; ++t) {
                sockaddr_in dest{};
                dest.sin_family      = AF_INET;
                dest.sin_port        = htons(kDiscoveryPort);
                dest.sin_addr.s_addr = htonl(targets[t]);
                sendto(sock, kProbeMessage, sizeof(kProbeMessage) - 1, 0,
                       (sockaddr*)&dest, sizeof(dest));
            }

            const uint64_t retryStart = Platform::NowMs();
            long lastResult = 0;
            int  lastErrno  = 0;

            while (Platform::NowMs() - retryStart < 2500) {
                char buf[2048];
                sockaddr_in from{};
                socklen_t fromLen = sizeof(from);
                const long n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                                        (sockaddr*)&from, &fromLen);
                lastResult = n;
                lastErrno  = (n < 0) ? SocketError() : 0;

                if (n <= 0) {
                    bj::SleepMs(50);
                    continue;
                }

                buf[n] = '\0';
                JsonDoc doc(std::string(buf, (size_t)n));
                if (!doc.valid()) continue;

                JfServer srv;
                srv.id      = doc["Id"].asString();
                srv.name    = doc["Name"].asString();
                srv.address = doc["Address"].asString();
                if (srv.address.empty()) {
                    char ip[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    if (ip[0]) srv.address = std::string("http://") + ip + ":8096";
                }
                if (srv.address.empty()) continue;
                if (srv.name.empty()) srv.name = "Jellyfin";

                LOGF("[discovery] retry found %s at %s",
                     srv.name.c_str(), srv.address.c_str());
                found.push_back(srv);
                break;
            }

            if (found.empty()) {
                LOGF("[discovery] retry gave up after %lums, last recvfrom=%ld errno=%d",
                     (unsigned long)(Platform::NowMs() - retryStart),
                     lastResult, lastErrno);
            }
        }
    }

    CLOSE_SOCKET(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    LOGF("[discovery] finished with %lu server(s)", (unsigned long)found.size());
    return found;
}
