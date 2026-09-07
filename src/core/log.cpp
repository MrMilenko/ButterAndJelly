// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/log.h"

#include "core/platform.h"
#include "core/thread.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#if defined(_XENON)
  // stderr goes nowhere on this console. XBDM's debug channel is where a log
  // line can actually be read while the thing being logged is on screen --
  // "make -f Makefile.xenon watch" on the host.
  #include <xtl.h>
#endif

namespace {

std::string g_path;
bj::Mutex  g_mutex;

}  // namespace

namespace Log {

void Init()
{
    bj::ScopedLock lock(g_mutex);
    g_path = Platform::DataDir() + "/log.txt";

    // Truncate the previous run.
    FILE* fp = std::fopen(Platform::NativePath(g_path).c_str(), "w");
    if (fp) {
        std::fprintf(fp, "%s %s\n", kAppName, kAppVersion);
        std::fclose(fp);
    }
}

void Shutdown()
{
    bj::ScopedLock lock(g_mutex);
    g_path.clear();
}

void Write(const char* fmt, ...)
{
    char buffer[512];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    bj::ScopedLock lock(g_mutex);
#if defined(_XENON)
    {
        char line[544];
        std::snprintf(line, sizeof(line), "%s\r\n", buffer);
        OutputDebugStringA(line);
    }
#else
    std::fprintf(stderr, "%s\n", buffer);
#endif
    if (g_path.empty()) return;

    // Reopened per line rather than held open. The console's FTP server
    // cannot read a file the running app still has open, and the log is only
    // useful if it can be read while the thing being debugged is on screen.
    FILE* fp = std::fopen(Platform::NativePath(g_path).c_str(), "a");
    if (!fp) return;
    std::fprintf(fp, "%s\n", buffer);
    std::fclose(fp);
}

}  // namespace Log
