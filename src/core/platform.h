// SPDX-License-Identifier: GPL-2.0-or-later

// platform.h: the small set of host services the portable code needs.

#pragma once

#include <cstdint>
#include <string>

#if defined(__WIIU__)
  #define BJ_PLATFORM_NAME "WiiU"
  #define BJ_PLATFORM_LONG "Wii U"
#elif defined(_XENON)
  #define BJ_PLATFORM_NAME "Xenon"
  #define BJ_PLATFORM_LONG "Xbox 360"
#elif defined(_XBOX)
  #define BJ_PLATFORM_NAME "Midway"
  #define BJ_PLATFORM_LONG "Xbox"
#elif defined(__APPLE__)
  #define BJ_PLATFORM_NAME "Darwin"
  #define BJ_PLATFORM_LONG "macOS"
#else
  #define BJ_PLATFORM_NAME "Desktop"
  #define BJ_PLATFORM_LONG "desktop"
#endif

inline constexpr const char* kPlatformName     = BJ_PLATFORM_NAME;
inline constexpr const char* kPlatformLongName = BJ_PLATFORM_LONG;

// The same name with its article, for running text. "the Wii U", but "macOS".
#if defined(__APPLE__)
  #define BJ_PLATFORM_THE BJ_PLATFORM_LONG
#else
  #define BJ_PLATFORM_THE "the " BJ_PLATFORM_LONG
#endif
inline constexpr const char* kPlatformArticleName = BJ_PLATFORM_THE;

// Identity we present to Jellyfin. The server shows DeviceName in its
// dashboard and in "Devices", so make it something recognizable on a TV.
inline constexpr const char* kAppName       = BJ_PLATFORM_NAME " Butter and Jelly";
inline constexpr const char* kAppShortName  = "ButterAndJelly";
inline constexpr const char* kAppVersion    = "0.1.3";

#define BJ_COPYRIGHT_YEAR "2026"
inline constexpr const char* kAppUserAgent  = "ButterAndJelly/0.1.3 (" BJ_PLATFORM_LONG ")";

namespace Platform {

// Called once at startup, before anything touches the filesystem.
void Init();

// Directory for settings, tokens, and the artwork cache. Created on demand,
// always returned without a trailing slash.
std::string DataDir();

// Read-only assets that ship with the build (fonts, CA bundle).
std::string AssetDir();

// Path to the bundled CA certificate list, or "" when the platform has a
// usable system store.
std::string CaBundlePath();

bool FileExists(const std::string& path);
bool MakeDirs(const std::string& path);

// Rewrites a path the way this machine's C library wants it. Everything above
// builds paths with forward slashes; the Xbox 360 addresses storage by device
// and spells the rest with backslashes, and its CRT will not take a mixture.
// The identity function everywhere else.
std::string NativePath(const std::string& path);

// Monotonic milliseconds since start. Used for polling cadence and UI timing.
uint64_t NowMs();

// This machine's IPv4 address in host byte order, or 0 if unknown. Used to
// derive a subnet-directed broadcast address, which some stacks deliver when
// they drop 255.255.255.255.
uint32_t LocalIPv4();

#if defined(_XBOX) && !defined(_XENON)
void LogMemory(const char* when);

// Logs once, the first time free memory falls below what the build was
// budgeted for. Silent on a machine that never gets there.
void WarnIfMemoryLow(const char* where);
#endif

// What the network link can be expected to carry in bits per second, or 0
// when it is not a constraint. Only the Xbox 360 answers: its wireless
// measures around 3.5 Mbit/s, under what a 720p transcode asks for.
uint32_t LinkBitrateCeiling();

// Writes what the machine's video hardware can do to the log. On the console
// this asks the H.264 block what it will accept; on the desktop it says
// nothing, because there playback goes through a software decoder.
void LogVideoCapabilities();

}  // namespace Platform
