// jfcli: a headless driver for the portable core.
//
// The console build and this tool call exactly the same client code, so a
// protocol bug can be found here in a second instead of on a TV.
//
//   jfcli discover
//   jfcli login <server-url>
//   jfcli whoami
//   jfcli libs
//   jfcli items <parentId> [IncludeItemTypes]
//   jfcli urls <itemId>
//   jfcli logout

#include "core/http.h"
#include "core/jellyfin.h"
#include "core/ts_demux.h"
#include "core/video_decoder.h"
#include "core/video_frame.h"
#include "core/platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

namespace {

std::string RuntimeShort(int seconds)
{
    if (seconds <= 0) return "-";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dm", seconds / 60);
    return buf;
}

void Usage()
{
    std::printf(
        "usage: jfcli <command> [args]\n"
        "  discover                       find servers on this network\n"
        "  login <server-url>             Quick Connect sign-in\n"
        "  whoami                         show the stored session\n"
        "  libs                           list libraries\n"
        "  items <parentId> [types]       list a library's contents\n"
        "  show <seriesId> [seasonId]     seasons, or one season's episodes\n"
        "  resume | nextup | latest       home screen rows\n"
        "  search <term>                  search the library\n"
        "  urls <itemId>                  print artwork and audio URLs\n"
        "  ts <file.ts>                   demux a transport stream and report\n"
        "  decode <file.ts> <out.ppm> [n] decode and colour convert frame n\n"
        "  logout                         drop the stored session\n");
}

int CmdDiscover()
{
    std::printf("Searching for Jellyfin servers...\n");
    const std::vector<JfServer> servers = JellyfinClient::Discover(2000);
    if (servers.empty()) {
        std::printf("No servers answered. The server may be on another subnet,\n"
                    "or auto-discovery may be turned off in its settings.\n");
        return 1;
    }
    for (const JfServer& s : servers) {
        std::printf("  %-28s %s\n", s.name.c_str(), s.address.c_str());
    }
    return 0;
}

int CmdLogin(const std::string& url)
{
    JellyfinClient jf;
    jf.setServerUrl(url);

    std::string serverName, error;
    if (!jf.ping(serverName, error)) {
        std::printf("Cannot reach %s: %s\n", jf.serverUrl().c_str(), error.c_str());
        return 1;
    }
    std::printf("Connected to \"%s\" at %s\n", serverName.c_str(), jf.serverUrl().c_str());

    std::string code;
    if (!jf.quickConnectStart(code, error)) {
        std::printf("Quick Connect unavailable: %s\n", error.c_str());
        return 1;
    }

    std::printf("\n  Quick Connect code: %s\n\n", code.c_str());
    std::printf("  Enter it in Jellyfin under your user menu -> Quick Connect.\n");
    std::fflush(stdout);

    const uint64_t deadline = Platform::NowMs() + 5 * 60 * 1000;
    while (Platform::NowMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const QuickConnectState state = jf.quickConnectPoll(error);
        if (state == QuickConnectState::Approved) break;
        if (state == QuickConnectState::Expired) {
            std::printf("The code expired. Run login again.\n");
            return 1;
        }
        std::printf(".");
        std::fflush(stdout);
    }
    std::printf("\n");

    if (!jf.quickConnectFinish(error)) {
        std::printf("Sign-in failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("Signed in as %s. Session saved to %s\n",
                jf.userName().c_str(), Platform::DataDir().c_str());
    return 0;
}

// Every command below needs a stored session; load it once here.
bool RequireSession(JellyfinClient& jf)
{
    if (jf.loadSession()) return true;
    std::printf("Not signed in. Run: jfcli login <server-url>\n");
    return false;
}

int CmdWhoami()
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;
    std::printf("server:   %s\n", jf.serverUrl().c_str());
    std::printf("user:     %s (%s)\n", jf.userName().c_str(), jf.userId().c_str());
    std::printf("deviceId: %s\n", jf.deviceId().c_str());
    return 0;
}

int CmdLibs()
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;

    std::string error;
    const std::vector<JfLibrary> libs = jf.libraries(error);
    if (libs.empty()) {
        std::printf("No libraries returned%s%s\n",
                    error.empty() ? "" : ": ", error.c_str());
        return 1;
    }
    for (const JfLibrary& lib : libs) {
        std::printf("  %-36s  %-12s  %s\n",
                    lib.id.c_str(), lib.collectionType.c_str(), lib.name.c_str());
    }
    return 0;
}

int CmdItems(const std::string& parentId, const std::string& types)
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;

    JfQuery q;
    q.parentId         = parentId;
    q.includeItemTypes = types;
    q.limit            = 100;

    std::string error;
    const std::vector<JfItem> items = jf.items(q, error);
    if (items.empty()) {
        std::printf("No items returned%s%s\n",
                    error.empty() ? "" : ": ", error.c_str());
        return 1;
    }
    for (const JfItem& it : items) {
        std::printf("  %-36s  %-12s  %-4d  %s\n",
                    it.id.c_str(), it.type.c_str(),
                    it.productionYear, it.name.c_str());
    }
    std::printf("(%zu items)\n", items.size());
    return 0;
}

// Prints a series' seasons, or one season's episodes.
int CmdShow(const std::string& seriesId, const std::string& seasonId)
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;

    std::string error;
    if (seasonId.empty()) {
        const std::vector<JfItem> seasons = jf.seasons(seriesId, error);
        if (seasons.empty()) {
            std::printf("No seasons%s%s\n", error.empty() ? "" : ": ", error.c_str());
            return 1;
        }
        for (const JfItem& s : seasons) {
            std::printf("  %-36s  S%-3d  %s\n",
                        s.id.c_str(), s.indexNumber, s.name.c_str());
        }
        return 0;
    }

    const std::vector<JfItem> eps = jf.episodes(seriesId, seasonId, error);
    if (eps.empty()) {
        std::printf("No episodes%s%s\n", error.empty() ? "" : ": ", error.c_str());
        return 1;
    }
    for (const JfItem& e : eps) {
        std::printf("  %-36s  S%02dE%02d  %-6s  %s\n",
                    e.id.c_str(), e.parentIndexNumber, e.indexNumber,
                    RuntimeShort(e.runtimeSeconds()).c_str(), e.name.c_str());
    }
    std::printf("(%zu episodes)\n", eps.size());
    return 0;
}

int CmdUrls(const std::string& itemId)
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;

    JfItem it;
    std::string error;
    if (!jf.itemDetails(itemId, it, error)) {
        std::printf("Lookup failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("name:   %s\n", it.name.c_str());
    std::printf("type:   %s\n", it.type.c_str());
    std::printf("length: %d s\n", it.runtimeSeconds());
    std::printf("art:    %s\n", jf.imageUrl(it.id, it.primaryTag, 400).c_str());
    std::printf("pcm:    %s\n", jf.audioPcmUrl(it.id).c_str());
    std::printf("direct: %s\n", jf.audioDirectUrl(it.id).c_str());
    return 0;
}

// Demuxes a .ts file and reports what came out. Ground truth for this is
// ffprobe on the same file.
int CmdTs(const std::string& path)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { std::printf("cannot open %s\n", path.c_str()); return 1; }

    TsDemuxer demux;
    size_t videoSamples = 0, audioSamples = 0;
    size_t videoBytes = 0, audioBytes = 0;
    int64_t firstPts = -1, lastPts = -1;
    size_t idrCount = 0, annexBOk = 0;

    auto onSample = [&](const TsSample& s) {
        if (s.video) {
            ++videoSamples;
            videoBytes += s.data.size();
            if (s.pts >= 0) {
                if (firstPts < 0) firstPts = s.pts;
                lastPts = s.pts;
            }
            // A sample is Annex-B if it opens with a start code, which is
            // three bytes (00 00 01) or four (00 00 00 01).
            if (s.data.size() >= 4 && s.data[0] == 0 && s.data[1] == 0 &&
                ((s.data[2] == 1) || (s.data[2] == 0 && s.data[3] == 1))) {
                ++annexBOk;
            }
            for (size_t i = 0; i + 4 < s.data.size(); ++i) {
                if (!(s.data[i] == 0 && s.data[i+1] == 0 && s.data[i+2] == 1)) continue;
                if ((s.data[i+3] & 0x1F) == 5) { ++idrCount; break; }
            }
        } else {
            ++audioSamples;
            audioBytes += s.data.size();
        }
    };

    std::vector<uint8_t> buffer(64 * 1024);
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0) {
        demux.feed(buffer.data(), n, onSample);
    }
    demux.flush(onSample);
    std::fclose(fp);

    std::printf("packets:      %llu seen, %llu dropped\n",
                (unsigned long long)demux.packetsSeen(),
                (unsigned long long)demux.packetsDropped());
    std::printf("video pid:    %d (stream type 0x%02X)\n",
                demux.videoPid(), demux.videoStreamType());
    std::printf("audio pid:    %d (stream type 0x%02X)\n",
                demux.audioPid(), demux.audioStreamType());
    std::printf("video:        %zu samples, %zu bytes, %zu start with Annex-B, %zu IDR\n",
                videoSamples, videoBytes, annexBOk, idrCount);
    // PES packets, not codec frames: one MP3 PES packet holds several
    // MP3 frames, which the audio decoder splits out later.
    std::printf("audio:        %zu PES packets, %zu bytes\n", audioSamples, audioBytes);
    if (firstPts >= 0) {
        const double seconds = (double)(lastPts - firstPts) / 90000.0;
        std::printf("video pts:    %lld -> %lld (%.3f s)\n",
                    (long long)firstPts, (long long)lastPts, seconds);
    }
    return 0;
}

// Runs a transport stream all the way through decode and colour conversion,
// writing one frame out as a PPM. Proves the whole chain in one command.
int CmdDecode(const std::string& path, const std::string& outPath, int wantFrame)
{
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { std::printf("cannot open %s\n", path.c_str()); return 1; }

    std::unique_ptr<VideoDecoder> decoder = VideoDecoder::Create();
    std::string error;
    if (!decoder->open(1920, 1080, error)) {
        std::printf("decoder: %s\n", error.c_str());
        std::fclose(fp);
        return 1;
    }
    std::printf("decoder:      %s\n", decoder->name());

    int frameCount = 0;
    int savedWidth = 0, savedHeight = 0;
    std::vector<uint8_t> rgba;
    double convertMsTotal = 0.0;
    double convertMsThreaded = 0.0;

    Nv12Frame kept;
    int64_t prevPts = INT64_MIN;
    int missingPts = 0, backwardsPts = 0;
    int64_t minDelta = INT64_MAX, maxDelta = INT64_MIN;
    auto onFrame = [&](const Nv12View& view) {
        // Presentation timing depends entirely on these being present and
        // ascending; anything else makes the player drop frames it should
        // have shown.
        if (view.pts < 0) {
            ++missingPts;
        } else if (prevPts != INT64_MIN) {
            const int64_t delta = view.pts - prevPts;
            if (delta < 0) ++backwardsPts;
            if (delta < minDelta) minDelta = delta;
            if (delta > maxDelta) maxDelta = delta;
        }
        if (view.pts >= 0) prevPts = view.pts;

        if (frameCount == wantFrame) {
            kept.copyFrom(view);
            const Nv12Frame& frame = kept;
            rgba.assign((size_t)frame.width * frame.height * 4, 0);
            const ColorSpace space = ColorSpaceForHeight(frame.height);

            // One conversion is far below the clock's resolution, so time a
            // batch. This number is what gets scaled to guess at the console.
            constexpr int kRuns = 50;
            const uint64_t start = Platform::NowMs();
            for (int run = 0; run < kRuns; ++run) {
                ConvertNv12ToRgba(frame, rgba.data(), frame.width * 4, space);
            }
            convertMsTotal = (double)(Platform::NowMs() - start) / kRuns;

            Nv12Converter converter(3);
            const uint64_t startMt = Platform::NowMs();
            for (int run = 0; run < kRuns; ++run) {
                converter.convert(frame, rgba.data(), frame.width * 4, space);
            }
            convertMsThreaded = (double)(Platform::NowMs() - startMt) / kRuns;

            savedWidth  = frame.width;
            savedHeight = frame.height;
        }
        ++frameCount;
    };

    TsDemuxer demux;
    auto onSample = [&](const TsSample& s) {
        if (!s.video) return;
        decoder->decode(s.data.data(), s.data.size(), s.pts, onFrame);
    };

    std::vector<uint8_t> buffer(64 * 1024);
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0) {
        demux.feed(buffer.data(), n, onSample);
    }
    demux.flush(onSample);
    decoder->flush(onFrame);
    std::fclose(fp);

    std::printf("frames:       %d decoded\n", frameCount);
    std::printf("pts:          %d missing, %d out of order\n", missingPts, backwardsPts);
    if (minDelta != INT64_MAX) {
        std::printf("pts deltas:   %lld min, %lld max (90kHz; 3754 = 23.976fps)\n",
                    (long long)minDelta, (long long)maxDelta);
    }
    if (rgba.empty()) { std::printf("frame %d never arrived\n", wantFrame); return 1; }

    std::printf("frame %d:      %dx%d\n", wantFrame, savedWidth, savedHeight);
    std::printf("convert:      %.2f ms single thread, %.2f ms across 3\n",
                convertMsTotal, convertMsThreaded);

    // PPM: a header and raw RGB. Nothing here needs a PNG encoder.
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) { std::printf("cannot write %s\n", outPath.c_str()); return 1; }
    std::fprintf(out, "P6\n%d %d\n255\n", savedWidth, savedHeight);
    for (int y = 0; y < savedHeight; ++y) {
        for (int x = 0; x < savedWidth; ++x) {
            std::fwrite(&rgba[((size_t)y * savedWidth + x) * 4], 1, 3, out);
        }
    }
    std::fclose(out);
    std::printf("wrote:        %s\n", outPath.c_str());
    return 0;
}

// The rows a home screen is made of, plus search.
int CmdRows(const std::string& which, const std::string& term)
{
    JellyfinClient jf;
    if (!RequireSession(jf)) return 1;

    std::string error;
    std::vector<JfItem> items;
    if      (which == "resume") items = jf.resumable(error);
    else if (which == "nextup") items = jf.nextUp(error);
    else if (which == "latest") items = jf.recentlyAdded(error);
    else if (which == "search") items = jf.search(term, error);
    else { std::printf("unknown row %s\n", which.c_str()); return 2; }

    if (items.empty()) {
        std::printf("nothing returned%s%s\n", error.empty() ? "" : ": ", error.c_str());
        return 1;
    }
    for (const JfItem& it : items) {
        char progress[32] = "";
        if (it.partiallyWatched()) {
            std::snprintf(progress, sizeof(progress), "  %3.0f%% (%ds in)",
                          it.playedFraction * 100.0, it.resumeSeconds());
        } else if (it.played) {
            std::snprintf(progress, sizeof(progress), "  watched");
        }
        std::printf("  %-11s %-42s%s\n", it.type.c_str(),
                    (it.seriesName.empty() ? it.name
                        : it.seriesName + " - " + it.name).substr(0, 42).c_str(),
                    progress);
    }
    std::printf("(%zu)\n", items.size());
    return 0;
}

int CmdLogout()
{
    JellyfinClient jf;
    if (!jf.loadSession()) { std::printf("Nothing to sign out of.\n"); return 0; }
    jf.signOut();
    std::printf("Signed out.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) { Usage(); return 2; }

    Platform::Init();
    if (!Http::Init()) {
        std::printf("Network unavailable.\n");
        return 1;
    }

    const std::string cmd = argv[1];
    int rc = 2;

    if      (cmd == "discover") rc = CmdDiscover();
    else if (cmd == "login")    rc = (argc >= 3) ? CmdLogin(argv[2]) : (Usage(), 2);
    else if (cmd == "whoami")   rc = CmdWhoami();
    else if (cmd == "libs")     rc = CmdLibs();
    else if (cmd == "items")    rc = (argc >= 3) ? CmdItems(argv[2], argc >= 4 ? argv[3] : "") : (Usage(), 2);
    else if (cmd == "show")     rc = (argc >= 3) ? CmdShow(argv[2], argc >= 4 ? argv[3] : "") : (Usage(), 2);
    else if (cmd == "resume" || cmd == "nextup" || cmd == "latest")
                                rc = CmdRows(cmd, "");
    else if (cmd == "search")   rc = (argc >= 3) ? CmdRows("search", argv[2]) : (Usage(), 2);
    else if (cmd == "urls")     rc = (argc >= 3) ? CmdUrls(argv[2]) : (Usage(), 2);
    else if (cmd == "ts")       rc = (argc >= 3) ? CmdTs(argv[2]) : (Usage(), 2);
    else if (cmd == "decode")   rc = (argc >= 4) ? CmdDecode(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0) : (Usage(), 2);
    else if (cmd == "logout")   rc = CmdLogout();
    else Usage();

    Http::Shutdown();
    return rc;
}
