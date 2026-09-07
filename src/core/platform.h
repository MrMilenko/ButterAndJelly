// SPDX-License-Identifier: GPL-2.0-or-later

// platform.h: the small set of host services the portable code needs.
//
// Implemented twice: platform/desktop/platform_desktop.cpp writes under the
// user's application-support directory, platform/wiiu/platform_wiiu.cpp
// writes to the SD card next to the bundle.

#pragma once

#include <cstdint>
#include <string>

// The project is Butter and Jelly. Each console it runs on puts its own name
// in front, so a Wii U build is WiiU Butter and Jelly and an Xbox 360 build
// is Xenon Butter and Jelly. The name appears in the title bar, in the
// about screen, and in Jellyfin's device list, which is worth keeping
// distinct when a household has more than one console signed in.
#if defined(__WIIU__)
  #define BJ_PLATFORM_NAME "WiiU"
  #define BJ_PLATFORM_LONG "Wii U"
#elif defined(_XENON)
  #define BJ_PLATFORM_NAME "Xenon"
  #define BJ_PLATFORM_LONG "Xbox 360"
#else
  #define BJ_PLATFORM_NAME "Desktop"
  #define BJ_PLATFORM_LONG "desktop"
#endif

inline constexpr const char* kPlatformName     = BJ_PLATFORM_NAME;
inline constexpr const char* kPlatformLongName = BJ_PLATFORM_LONG;

// Identity we present to Jellyfin. The server shows DeviceName in its
// dashboard and in "Devices", so make it something recognisable on a TV.
inline constexpr const char* kAppName       = BJ_PLATFORM_NAME " Butter and Jelly";
inline constexpr const char* kAppShortName  = "ButterAndJelly";
inline constexpr const char* kAppVersion    = "0.1.2";
inline constexpr const char* kAppUserAgent  = "ButterAndJelly/0.1.2 (" BJ_PLATFORM_LONG ")";

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

// What this machine's network link can be expected to carry, in bits per
// second, or 0 when there is no reason to think it is a constraint.
//
// Only the Xbox 360 answers with anything: its built-in wireless measures
// around 3.5 Mbit/s in practice, which is less than a 720p transcode was
// being asked for, and the difference showed up as playback stalling every
// few seconds on high bitrate films while the decoder sat idle.
uint32_t LinkBitrateCeiling();

// Writes what the machine's video hardware can do to the log. On the console
// this asks the H.264 block what it will accept; on the desktop it says
// nothing, because there playback goes through a software decoder.
void LogVideoCapabilities();

}  // namespace Platform
