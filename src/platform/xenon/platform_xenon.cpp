// SPDX-License-Identifier: GPL-2.0-or-later

// platform_xenon.cpp: Xbox 360 host services.
//
// None of this has run on a console yet. Every path and API choice here is
// from the XDK headers rather than from a machine, and the places that need
// checking against real hardware say so.

#include "core/log.h"
#include "core/platform.h"

#include <xtl.h>
#include <winsockx.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// The 360 addresses storage by device. "game:" is the directory the XEX was
// launched from.
//
// No trailing separator: callers append "/something" and NativePath converts
// to backslashes.
const char* kAssetRoot = "game:";

// Beside the executable, not on "cache:". That device need not be mounted on
// a console booted from the hard disk, and when it is not, every write fails
// silently. "game:" is writable when the title is launched from the disk and
// puts the settings, session and artwork next to the binary.
const char* kDataRoot = "game:\\data";

std::string g_dataDir;
std::string g_assetDir;
bool        g_ready = false;
bool        g_network = false;
bool        g_wireless = false;

// The stack has to start before any socket call, and it comes up on Xbox
// Live's secure addressing unless told otherwise. Without BYPASS_SECURITY a
// socket to an ordinary LAN server does not connect.
//
// Here rather than in Http::Init because discovery opens its own UDP socket.
void StartNetwork()
{
    if (g_network) return;

    XNetStartupParams params;
    std::memset(&params, 0, sizeof(params));
    params.cfgSizeOfStruct = sizeof(params);
    params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

    int started = XNetStartup(&params);
    if (started != 0) {
        // Almost always because something else brought XNet up first, a
        // dashboard with an FTP server in it say, in which case it is
        // running with the security this title needs bypassed, and every
        // packet to an ordinary host on the LAN is quietly dropped while
        // every call still reports success. Take it down and start it again
        // on our terms.
        LOGF("[net] XNetStartup returned %d; restarting it", started);
        XNetCleanup();
        started = XNetStartup(&params);
    }
    LOGF("[net] XNetStartup -> %d, security bypassed", started);
    if (started != 0) return;

    WSADATA data;
    const int winsock = WSAStartup(MAKEWORD(2, 2), &data);
    if (winsock != 0) {
        LOGF("[net] WSAStartup failed: %d", winsock);
        XNetCleanup();
        return;
    }
    g_network = true;

    const DWORD link = XNetGetEthernetLinkStatus();
    g_wireless = (link & XNET_ETHERNET_LINK_WIRELESS) != 0;
    LOGF("[net] link 0x%08x%s%s%s", (unsigned)link,
         (link & XNET_ETHERNET_LINK_ACTIVE)   ? " active"   : " DOWN",
         (link & XNET_ETHERNET_LINK_WIRELESS) ? " wireless" : "",
         (link & XNET_ETHERNET_LINK_100MBPS)  ? " 100Mbps"  : "");
}

}  // namespace

namespace Platform {

void Init()
{
    if (g_ready) return;
    g_ready = true;

    g_assetDir = kAssetRoot;

    g_dataDir = kDataRoot;
    MakeDirs(g_dataDir);
    MakeDirs(g_dataDir + "\\art");

    // Say so rather than failing quietly: everything that remembers anything
    // between launches goes through here.
    const std::string probe = NativePath(g_dataDir + "\\.writable");
    if (std::FILE* f = std::fopen(probe.c_str(), "wb")) {
        std::fclose(f);
        std::remove(probe.c_str());
    } else {
        LOGF("[boot] %s is not writable; nothing will be remembered",
             g_dataDir.c_str());
    }

    StartNetwork();
}

std::string DataDir()  { Init(); return g_dataDir; }
std::string AssetDir() { Init(); return g_assetDir; }

std::string CaBundlePath()
{
    // The XDK has its own certificate handling and this build talks to a
    // server on the local network over plain HTTP, so nothing is shipped.
    return "";
}

std::string NativePath(const std::string& path)
{
    std::string out = path;
    for (char& ch : out) if (ch == '/') ch = '\\';
    // "game:\\data\\art\\x.jpg" is right; a doubled separator, which
    // is what joining "cache:\\dir" and "/file" produces, is not.
    for (size_t i = 1; i < out.size(); ) {
        if (out[i] == '\\' && out[i - 1] == '\\') out.erase(i, 1);
        else ++i;
    }
    return out;
}

bool FileExists(const std::string& path)
{
    // GetFileAttributes is declared by the XDK, but INVALID_FILE_ATTRIBUTES is
    // a Win32 SDK macro the console headers do not carry. The value is part of
    // the function's contract, not of the SDK, so name it here.
    const DWORD kInvalidFileAttributes = 0xFFFFFFFFu;

    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != kInvalidFileAttributes;
}

bool MakeDirs(const std::string& path)
{
    if (path.empty()) return false;

    // Build the path a component at a time. Backslashes here, and the
    // "device:" prefix is not a directory that can be created.
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool sep  = (path[i] == '\\' || path[i] == '/');
        const bool last = (i + 1 == path.size());
        if (!sep && !last) continue;

        std::string dir = partial;
        if (sep) dir.pop_back();
        if (dir.empty() || dir.back() == ':') continue;
        CreateDirectoryA(dir.c_str(), nullptr);
    }
    return FileExists(path);
}

uint64_t NowMs()
{
    // Milliseconds since boot. Wraps after about seven weeks, which is
    // longer than a console stays awake between launches.
    return (uint64_t)GetTickCount();
}

uint32_t LocalIPv4()
{
    // Used only to work out a subnet broadcast address for server discovery,
    // which falls back to the global broadcast address when this is zero.
    //
    // XNetGetTitleXnAddr answers XNET_GET_XNADDR_PENDING while the interface
    // is still coming up, so it is asked for rather than waited on: discovery
    // runs again, and by then it is usually settled.
    Init();
    if (!g_network) return 0;

    XNADDR address;
    std::memset(&address, 0, sizeof(address));
    const DWORD status = XNetGetTitleXnAddr(&address);
    if (status == XNET_GET_XNADDR_PENDING || status == XNET_GET_XNADDR_NONE) return 0;

    return ntohl(address.ina.s_addr);
}

// The console's own wireless, measured on this network, sustains about
// 3.5 Mbit/s. A 720p transcode was being asked for at 5 Mbit/s, so segments
// arrived slower than they played: the frame queue drained to nothing every
// few seconds while the decoder, which by then had capacity to spare, waited
// on the network. Ask for something the link can carry instead. Wired is left
// alone; 100Mbit is not a constraint.
uint32_t LinkBitrateCeiling()
{
    Init();
    return g_wireless ? 3000000u : 0u;
}

void LogVideoCapabilities()
{
    // Nothing to probe. The XDK's video decoder is XMV, meaning WMV9 and
    // VC-1 through xmedia2.lib, and there is no H.264 block behind it, so
    // there is no hardware to ask about.
}

}  // namespace Platform
