// SPDX-License-Identifier: GPL-2.0-or-later

// platform_desktop.cpp: macOS/Linux/Windows host services.

#include "core/platform.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif

namespace {

std::string g_dataDir;
std::string g_assetDir;

std::string HomeDir()
{
    if (const char* h = std::getenv("HOME")) return h;
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE")) return p;
#endif
    return ".";
}

}  // namespace

namespace Platform {

void Init()
{
    if (!g_dataDir.empty()) return;

#if defined(__APPLE__)
    g_dataDir = HomeDir() + "/Library/Application Support/" + kAppShortName;
#elif defined(_WIN32)
    const char* appData = std::getenv("APPDATA");
    g_dataDir = (appData ? std::string(appData) : HomeDir()) + "\\" + kAppShortName;
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    g_dataDir = xdg ? std::string(xdg) + "/" + kAppShortName
                    : HomeDir() + "/.local/share/" + kAppShortName;
#endif
    MakeDirs(g_dataDir);
    MakeDirs(g_dataDir + "/art");

    // Assets live next to the source tree during development; an installed
    // build can override this without touching the portable code.
    if (const char* override = std::getenv("BUTTERANDJELLY_ASSETS")) {
        g_assetDir = override;
        return;
    }
#if defined(__APPLE__)
    // A bundle's working directory is not the source tree.
    {
        char path[4096];
        uint32_t size = sizeof(path);
        if (_NSGetExecutablePath(path, &size) == 0) {
            std::string exe(path);
            const size_t slash = exe.rfind('/');
            if (slash != std::string::npos) {
                const std::string dir = exe.substr(0, slash);
                const std::string bundled = dir + "/../Resources/assets";
                struct stat info;
                if (stat(bundled.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
                    g_assetDir = bundled;
                    return;
                }
            }
        }
    }
#endif
    g_assetDir = "assets";
}

std::string DataDir()  { Init(); return g_dataDir; }
std::string AssetDir() { Init(); return g_assetDir; }

std::string CaBundlePath()
{
    // Desktop platforms have a system trust store curl already knows about.
    return "";
}

// Both of these spell paths the way the code above already does.
std::string NativePath(const std::string& path) { return path; }

bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size >= 0;
}

bool MakeDirs(const std::string& path)
{
    if (path.empty()) return false;
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool sep  = (path[i] == '/' || path[i] == '\\');
        const bool last = (i + 1 == path.size());
        if ((sep || last) && partial.size() > 1) {
            std::string dir = partial;
            if (sep) dir.pop_back();
            if (!dir.empty()) MKDIR(dir.c_str());
        }
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint32_t LocalIPv4()
{
#ifdef _WIN32
    return 0;   // the desktop path only needs this on POSIX
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return 0;

    uint32_t found = 0;
    for (ifaddrs* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* addr = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        found = ntohl(addr->sin_addr.s_addr);
        break;
    }
    freeifaddrs(list);
    return found;
#endif
}

// Wired, or a wireless link fast enough not to matter.
uint32_t LinkBitrateCeiling()
{
    return 0;
}

void LogVideoCapabilities()
{
    // Nothing to probe: the desktop build decodes in software.
}

uint64_t NowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

}  // namespace Platform
