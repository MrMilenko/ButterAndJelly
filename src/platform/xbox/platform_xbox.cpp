// SPDX-License-Identifier: GPL-2.0-or-later

// Original Xbox. Everything lives beside the executable; the console's own
// storage is not used.

#include "core/platform.h"
#include "core/log.h"

#include <xtl.h>
#include <winsockx.h>

#include <cstdio>
#include <cstring>

namespace {

// D: is the directory the XBE was launched from. Assets and data both sit
// there, so a title is one folder that can be copied anywhere.
const char* kAssetRoot = "D:";
const char* kDataRoot  = "D:\\data";

std::string g_dataDir;
std::string g_assetDir;
bool        g_ready   = false;
bool        g_network = false;

void StartNetwork()
{
    if (g_network) return;

    // Without BYPASS_SECURITY the stack only talks to other consoles and to
    // Xbox Live, and a socket to a server on the LAN does not connect.
    XNetStartupParams params;
    std::memset(&params, 0, sizeof(params));
    params.cfgSizeOfStruct = sizeof(params);
    params.cfgFlags        = XNET_STARTUP_BYPASS_SECURITY;

    // Pre-allocated here and never grown, so these decide throughput for the
    // whole session. The defaults are sized for a game trading small packets
    // and drop most of a video stream. Single bytes, so 255 is the ceiling.
    params.cfgPrivatePoolSizeInPages    = 128;  // 512K, from 48K
    params.cfgSockDefaultRecvBufsizeInK = 64;   // per socket, from 16K
    params.cfgSockDefaultSendBufsizeInK = 32;   // from 16K
    params.cfgEnetReceiveQueueLength    = 32;   // 64K of packets, from 16K
    params.cfgSockMaxSockets            = 32;   // fewer, but each much larger

    const INT rc = XNetStartup(&params);
    if (rc != 0) {
        LOGF("[net] XNetStartup failed: %d", rc);
        return;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGF("[net] WSAStartup failed");
        XNetCleanup();
        return;
    }
    g_network = true;
    LOGF("[net] XNetStartup ok, security bypassed, %dK pool, %dK per socket",
         params.cfgPrivatePoolSizeInPages * 4, params.cfgSockDefaultRecvBufsizeInK);
}

}  // namespace

namespace Platform {

// The XBE's own 64MB limit flag decides whether a 128MB console is seen
// as one.
// Stock hardware is 64MB. Eight of them left is the point at which the next
// allocation is worth knowing about.
void WarnIfMemoryLow(const char* where)
{
    static bool warned = false;
    if (warned) return;

    MEMORYSTATUS status;
    std::memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(status);
    GlobalMemoryStatus(&status);
    if (status.dwAvailPhys >= 8u * 1024 * 1024) return;

    warned = true;
    LOGF("[mem] low at %s: %lu KB free of %lu KB",
         where,
         (unsigned long)(status.dwAvailPhys / 1024),
         (unsigned long)(status.dwTotalPhys / 1024));
}

void LogMemory(const char* when)
{
    MEMORYSTATUS status;
    std::memset(&status, 0, sizeof(status));
    status.dwLength = sizeof(status);
    GlobalMemoryStatus(&status);
    LOGF("[mem] %s: %lu KB free of %lu KB",
         when,
         (unsigned long)(status.dwAvailPhys / 1024),
         (unsigned long)(status.dwTotalPhys / 1024));
}

void Init()
{
    if (g_ready) return;
    g_ready = true;

    g_assetDir = kAssetRoot;
    g_dataDir  = kDataRoot;
    MakeDirs(g_dataDir);
    MakeDirs(g_dataDir + "\\art");

    LogMemory("startup");
    StartNetwork();
}

std::string DataDir()  { Init(); return g_dataDir; }
std::string AssetDir() { Init(); return g_assetDir; }

// No TLS on this console, so nothing to point at.
std::string CaBundlePath() { return ""; }

std::string NativePath(const std::string& path)
{
    std::string out = path;
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == '/') out[i] = '\\';
    for (size_t i = 1; i < out.size(); ) {
        if (out[i] == '\\' && out[i - 1] == '\\') out.erase(i, 1);
        else ++i;
    }
    return out;
}

bool FileExists(const std::string& path)
{
    const DWORD attributes = GetFileAttributesA(NativePath(path).c_str());
    return attributes != 0xFFFFFFFFu;
}

bool MakeDirs(const std::string& path)
{
    if (path.empty()) return false;

    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool sep  = (path[i] == '\\' || path[i] == '/');
        const bool last = (i + 1 == path.size());
        if (!sep && !last) continue;

        std::string dir = partial;
        if (sep) dir.erase(dir.size() - 1);
        if (dir.empty() || dir[dir.size() - 1] == ':') continue;
        CreateDirectoryA(dir.c_str(), NULL);
    }
    return FileExists(path);
}

uint64_t NowMs()
{
    return (uint64_t)GetTickCount();
}

uint32_t LocalIPv4()
{
    Init();
    if (!g_network) return 0;

    XNADDR address;
    std::memset(&address, 0, sizeof(address));
    if (XNetGetTitleXnAddr(&address) == XNET_GET_XNADDR_PENDING) return 0;
    return ntohl(address.ina.s_addr);
}

// 100Mbit wired, so the link is never the limit.
uint32_t LinkBitrateCeiling() { return 0; }

void LogVideoCapabilities()
{
    LOGF("[video] software decode, 480p ceiling");
}

}  // namespace Platform
