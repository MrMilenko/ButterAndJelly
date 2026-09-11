// SPDX-License-Identifier: GPL-2.0-or-later

// platform_wiiu.cpp: Cafe OS host services.
//
// Assets are read from the mounted bundle (/vol/content) so a .wuhb stays
// self-contained. Anything we write goes to a plain folder on the SD card
// instead of the bundle's save area, so the user can pull the log or clear
// the art cache by putting the card in a PC.

#include "core/platform.h"

#include <coreinit/time.h>
#include <h264/decode.h>
#include <nn/ac.h>
#include <nn/result.h>

#include "core/log.h"
#include "core/video_frame.h"
#include "core/text.h"

#include <cstdio>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

// Deliberately still "butterjelly": this is where existing Wii U installs
// keep their settings, sign-in and poster cache. Renaming the folder would
// orphan all of it and sign every current tester out, which is not worth
// matching the executable's name.
const char* kSdDataDir  = "fs:/vol/external01/wiiu/butterjelly";
const char* kBundleRoot = "/vol/content";

std::string g_dataDir;
std::string g_assetDir;
bool        g_ready = false;

}  // namespace

namespace Platform {

void Init()
{
    if (g_ready) return;
    g_ready = true;

    g_dataDir = kSdDataDir;
    MakeDirs(g_dataDir);
    MakeDirs(g_dataDir + "/art");

    // Running from a .wuhb gives us /vol/content. A bare .rpx launched from
    // the Homebrew Launcher does not, so fall back to the SD folder and let
    // the user drop the assets there.
    struct stat st;
    if (stat(kBundleRoot, &st) == 0 && S_ISDIR(st.st_mode)) {
        g_assetDir = kBundleRoot;
    } else {
        g_assetDir = g_dataDir + "/assets";
    }
}

std::string DataDir()  { Init(); return g_dataDir; }
std::string AssetDir() { Init(); return g_assetDir; }

std::string CaBundlePath()
{
    // Cafe OS has no trust store curl can use, so HTTPS depends entirely on
    // the bundle we ship.
    Init();
    return g_assetDir + "/cacert.pem";
}

// Both of these spell paths the way the code above already does.
std::string NativePath(const std::string& path) { return path; }

bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool MakeDirs(const std::string& path)
{
    if (path.empty()) return false;
    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool sep  = (path[i] == '/');
        const bool last = (i + 1 == path.size());
        if ((sep || last) && partial.size() > 1) {
            std::string dir = partial;
            if (sep) dir.pop_back();
            // Skip the "fs:" prefix and the volume root, which always exist.
            if (dir.empty() || dir == "fs:" || dir == "fs:/") continue;
            mkdir(dir.c_str(), 0777);
        }
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t NowMs()
{
    return (uint64_t)OSTicksToMilliseconds(OSGetSystemTime());
}

void LogVideoCapabilities()
{
    // Ask the decoder how much memory each profile and size would need.
    // A failure here is the cheapest possible way to find out that a stream
    // shape is not decodable, long before trying to play one.
    struct Probe { const char* label; int32_t profile; int32_t level;
                   int32_t width; int32_t height; };
    static const Probe probes[] = {
        { "baseline@3.1 854x480",  66, 31,  854,  480 },
        { "main@4.0 1280x720",     77, 40, 1280,  720 },
        { "high@4.1 1280x720",    100, 41, 1280,  720 },
        { "high@4.1 1920x1080",   100, 41, 1920, 1080 },
    };

    for (const Probe& p : probes) {
        uint32_t needed = 0;
        const H264Error err = H264DECMemoryRequirement(p.profile, p.level,
                                                       p.width, p.height, &needed);
        if (err == H264_ERROR_OK) {
            LOGF("[video] %s: ok, needs %u KB", p.label, needed / 1024);
        } else {
            LOGF("[video] %s: rejected (error 0x%X)", p.label, (unsigned)err);
        }
    }

    // The renderer takes RGB only, so every decoded frame has to be color
    // converted on this CPU. Whether that fits in a frame's budget is the
    // question the whole playback design turns on, so measure it here rather
    // than guess from desktop numbers.
    {
        const int width = 1280, height = 694;
        Nv12Frame probe;
        probe.width  = width;
        probe.height = height;
        probe.lumaStride = probe.chromaStride = width;
        probe.luma.assign((size_t)width * height, 128);
        probe.chroma.assign((size_t)width * (height / 2), 128);

        std::vector<uint8_t> rgba((size_t)width * height * 4);

        for (int threads : { 1, 2, 3 }) {
            constexpr int kRuns = 10;
            Nv12Converter converter(threads);
            const uint64_t start = NowMs();
            for (int run = 0; run < kRuns; ++run) {
                converter.convert(probe, rgba.data(), width * 4, ColorSpace::Bt709);
            }
            const double perFrame = (double)(NowMs() - start) / kRuns;
            // 23.976fps leaves 41.7ms per frame for everything, not just this.
            LOGF("[video] convert %dx%d on %d thread(s): %s ms/frame (budget 41.7)",
                 width, height, threads, bj::ToString(perFrame, 1).c_str());
        }
    }
}

uint32_t LocalIPv4()
{
    uint32_t ip = 0;
    if (NNResult_IsFailure(ACGetAssignedAddress(&ip))) return 0;
    return ip;   // already host byte order
}

// Wired, or a wireless link fast enough not to matter.
uint32_t LinkBitrateCeiling()
{
    return 0;
}

}  // namespace Platform
