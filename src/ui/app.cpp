// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/app.h"

#include "core/http.h"
#include "core/log.h"
#include "core/platform.h"
#include "ui/image_load.h"
#include "core/text.h"


#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kSidebarWidth = 300;
constexpr int kTopBarHeight = 74;
constexpr int kHintBarHeight = 56;
constexpr int kGridColumns  = 4;

// How a decoded frame reaches the screen. _XENON first: _XBOX is both Xboxes.
#if defined(_XENON)
constexpr Uint32     kVideoYuvFormat  = SDL_PIXELFORMAT_IYUV;
constexpr bool       kPreferYuvUpload = true;
constexpr Uint32     kVideoRgbFormat  = SDL_PIXELFORMAT_ARGB8888;
constexpr PixelOrder kVideoPixelOrder = PixelOrder::Argb;
#elif defined(_XBOX)
// YUY2, converted by the NV2A while sampling. Packing costs a byte shuffle
// instead of a color conversion and halves the upload. See PackNv12ToYuy2.
#define BJ_VIDEO_YUY2 1
constexpr Uint32     kVideoYuvFormat  = SDL_PIXELFORMAT_IYUV;
constexpr bool       kPreferYuvUpload = false;
constexpr Uint32     kVideoRgbFormat  = SDL_PIXELFORMAT_YUY2;
constexpr PixelOrder kVideoPixelOrder = PixelOrder::Bgra;
#else
constexpr Uint32     kVideoYuvFormat  = SDL_PIXELFORMAT_IYUV;
constexpr bool       kPreferYuvUpload = false;
constexpr Uint32     kVideoRgbFormat  = SDL_PIXELFORMAT_RGBA32;
constexpr PixelOrder kVideoPixelOrder = PixelOrder::Rgba;
#endif

// SDL accepts a YUV format it does not list and converts on the CPU, which
// is slower than doing it here.
bool RendererTakesYuv(SDL_Renderer* renderer)
{
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0) return false;
    for (Uint32 i = 0; i < info.num_texture_formats; ++i) {
        if (info.texture_formats[i] == kVideoYuvFormat) return true;
    }
    return false;
}

// The Wii U converts color on the CPU and cannot hold 720p in the frame
// budget; the 360 converts on the GPU and can.
#if defined(__WIIU__)
constexpr int kPlaybackMaxHeight = 480;
#elif defined(_XENON)
constexpr int kPlaybackMaxHeight = 720;
#elif defined(_XBOX)
constexpr int kPlaybackMaxHeight = 480;
#else
// Not a console. 0 asks the server for the source as it is.
constexpr int kPlaybackMaxHeight = 0;
#endif

// What the Quality row steps through. 0 means no ceiling at all.
#if defined(__WIIU__) || (defined(_XBOX) && !defined(_XENON))
constexpr int kQualitySteps[] = { 480, 360, 240 };
#elif defined(_XENON)
constexpr int kQualitySteps[] = { 720, 480, 360 };
#else
constexpr int kQualitySteps[] = { 0, 1080, 720, 480 };
#endif
constexpr int kQualityStepCount =
    (int)(sizeof(kQualitySteps) / sizeof(kQualitySteps[0]));


// What the bitrate rows step through, in kbit/s. 0 is the automatic choice,
// which follows the height.
constexpr int kVideoBitrateSteps[] = { 0, 1500, 2500, 4000, 8000, 16000 };
constexpr int kVideoBitrateStepCount =
    (int)(sizeof(kVideoBitrateSteps) / sizeof(kVideoBitrateSteps[0]));

constexpr int kAudioBitrateSteps[] = { 128, 192, 256, 320 };
constexpr int kAudioBitrateStepCount =
    (int)(sizeof(kAudioBitrateSteps) / sizeof(kAudioBitrateSteps[0]));

// 0 means the source's own rate.
constexpr int kFramerateSteps[] = { 0, 24, 30, 60 };
constexpr int kFramerateStepCount =
    (int)(sizeof(kFramerateSteps) / sizeof(kFramerateSteps[0]));

std::string VideoBitrateLabel(int kbps)
{
    if (kbps <= 0) return "automatic, from the quality";
    return bj::ToString(kbps / 1000) + "." +
           bj::ToString((kbps % 1000) / 100) + " Mbit/s";
}

std::string FramerateLabel(int fps)
{
    return fps <= 0 ? "whatever the source runs at"
                    : bj::ToString(fps) + " fps";
}

const char* AudioFormatLabel(Settings::AudioFormat format)
{
    switch (format) {
        case Settings::AudioFormat::Aac: return "AAC";
        case Settings::AudioFormat::Mp3: return "MP3";
        default: break;
    }
    return BJ_AUDIO_AAC ? "automatic, AAC where the source has it"
                        : "automatic, MP3";
}

// Status numbering is the API's own.
constexpr int kRequestPending    = 2;
constexpr int kRequestProcessing = 3;
constexpr int kRequestPartial    = 4;
constexpr int kRequestAvailable  = 5;

const char* RequestStatusLabel(int status)
{
    switch (status) {
        // Processing means approved and handed on, not downloading.
        case kRequestPending:
        case kRequestProcessing: return "Requested";
        case kRequestPartial:    return "Partly available";
        case kRequestAvailable:  return "Available";
        default: break;
    }
    return "";
}

// For listings that mix films and shows.
const char* MediaKindLabel(const JfItem& item)
{
    if (item.type == "Movie")  return "Movie";
    if (item.type == "Series") return "TV Show";
    return "";
}

// Quick Connect codes are short-lived; give up rather than poll forever.
constexpr uint64_t kSignInWindowMs = 5 * 60 * 1000;
constexpr uint64_t kPollIntervalMs = 2000;

// The consoles have one controller and no keyboard, so their hint labels are
// fixed at compile time.
#if defined(__WIIU__) || defined(_XBOX) || defined(__ORBIS__)
constexpr bool kFixedInputScheme = true;
#else
constexpr bool kFixedInputScheme = false;
#endif

// Title, two label lines and the gap to the next row.
constexpr int kHomeRowChrome = 90;

// Fetched a page at a time as the selection nears the end.
constexpr int kLibraryPageSize = 200;

// The sidebar under Seerr. `media` is empty where the endpoint takes no genre.
struct DiscoverMode { const char* path; const char* title; const char* media; };
const DiscoverMode kDiscoverModes[] = {
    { "trending",                                         "Trending",         ""      },
    { "movies",                                           "Popular films",    "movie" },
    { "tv",                                               "Popular shows",    "tv"    },
    { "movies?sortBy=vote_average.desc&voteCountGte=500", "Best rated films", "movie" },
    { "tv?sortBy=vote_average.desc&voteCountGte=200",     "Best rated shows", "tv"    },
    { "movies/upcoming",                                  "Coming soon",      ""      },
    { "tv/upcoming",                                      "New shows",        ""      },
};
constexpr int kDiscoverModeCount =
    (int)(sizeof(kDiscoverModes) / sizeof(kDiscoverModes[0]));

struct RequestFilter { const char* filter; const char* title; };
const RequestFilter kRequestFilters[] = {
    { "all",       "Requested"  },
    { "pending",   "Waiting"    },
    { "available", "Landed"     },
};
constexpr int kRequestFilterCount =
    (int)(sizeof(kRequestFilters) / sizeof(kRequestFilters[0]));


// Stills want 16:9 and posters 2:3. A mixed row follows the stills.
bool RowHasEpisodes(const HomeRow& row)
{
    for (const JfItem& item : row.items) {
        if (item.type == "Episode") return true;
    }
    return false;
}

std::string YearText(int year)
{
    if (year <= 0) return "";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", year);
    return buf;
}

// "1h 42m" / "42m" / "" - runtimes read better than raw minutes on a TV.
std::string RuntimeText(int seconds)
{
    if (seconds <= 0) return "";
    const int totalMinutes = seconds / 60;
    const int hours = totalMinutes / 60;
    const int minutes = totalMinutes % 60;
    char buf[32];
    if (hours > 0) std::snprintf(buf, sizeof(buf), "%dh %dm", hours, minutes);
    else           std::snprintf(buf, sizeof(buf), "%dm", minutes);
    return buf;
}

// Servers listed in DataDir()/server.txt, one base URL per line, for when
// broadcast does not reach.
std::vector<JfServer> ReadServerOverrides()
{
    std::vector<JfServer> out;

    // Beside the settings first, then beside the executable. The 360's data
    // directory is a cache device the debug bridge cannot write to.
    const std::string candidates[] = {
        Platform::DataDir()  + "/server.txt",
        Platform::AssetDir() + "/server.txt",
    };

    FILE* fp = nullptr;
    for (const std::string& candidate : candidates) {
        fp = std::fopen(Platform::NativePath(candidate).c_str(), "r");
        if (fp) break;
    }
    if (!fp) return out;

    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string url(line);
        while (!url.empty() && (url.back() == '\n' || url.back() == '\r' ||
                                url.back() == ' '  || url.back() == '\t')) {
            url.pop_back();
        }
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) {
            url.erase(url.begin());
        }
        if (url.empty() || url[0] == '#') continue;
        if (url.find("://") == std::string::npos) url = "http://" + url;

        JfServer srv;
        srv.address = url;
        srv.name    = "Saved server";
        out.push_back(srv);
    }
    std::fclose(fp);
    return out;
}

// Music libraries open on albums rather than every track.
std::string DefaultItemTypesFor(const std::string& collectionType)
{
    if (collectionType == "music")  return "MusicAlbum";
    if (collectionType == "movies") return "Movie";
    if (collectionType == "tvshows") return "Series";
    return "";   // let the server decide
}

}  // namespace

// ------------------------------------------------------------- lifecycle

App::App() = default;

App::~App()
{
    shutdown();
}

bool App::init(SDL_Window* window, SDL_Renderer* renderer)
{
    window_ = window;
    sdl_    = renderer;

    fontDir_ = Platform::AssetDir() + "/fonts";
    if (!render_.init(renderer, fontDir_)) {
        errorLine_ = "Could not load fonts from " + fontDir_;
        LOGF("[ui] %s", errorLine_.c_str());
        return false;
    }
    LOGF("[ui] fonts loaded from %s", fontDir_.c_str());

    // Optional: the service tabs only appear when this finds a server.
    seerr_.loadConfig();
    applyStreamPrefs();

    loadBarIcons();

    // Input glyphs are optional: the hint bar falls back to words.
    if (render_.setIconFont(fontDir_ + "/" + HintFontFile(inputScheme_))) {
        LOGF("[ui] input glyphs: %s", HintFontFile(inputScheme_));
    }

    art_ = std::unique_ptr<ArtCache>(new ArtCache(renderer, client_, pool_));

#ifdef __WIIU__
    // Falling back to the CPU path costs about 25ms a frame. A shader on the
    // SD card wins, so one can be tried without reinstalling.
    std::string gx2Error;
    const std::string overrideShader = Platform::DataDir() + "/shader.gsh";
    const std::string shaderPath = Platform::FileExists(overrideShader)
                                 ? overrideShader
                                 : Platform::AssetDir() + "/shaders/nv12.gsh";
    LOGF("[gx2] shader: %s", shaderPath.c_str());
    gx2Ready_ = gx2Video_.init(shaderPath, gx2Error);
    if (!gx2Ready_) LOGF("[gx2] unavailable, using the CPU path: %s", gx2Error.c_str());
#endif

    updateLayoutSize();
    LOGF("[ui] drawing at %dx%d", logicalW_, logicalH_);

    startup();
    return true;
}

void App::shutdown()
{
    stopPlayback();

    // The console does not give an app long to quit, and a slow download
    // would outlast it.
    Http::RequestAbort();

    // Order matters: stop the workers before the textures they feed go away.
    pool_.shutdown();
    art_.reset();
    render_.shutdown();
}

void App::setSettings(const Settings& settings)
{
    settings_ = settings;
}

void App::toast(const std::string& message)
{
    toastText_    = message;
    toastUntilMs_ = Platform::NowMs() + 2600;
}

bool App::writeScreenshot(const std::string& path)
{
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(sdl_, &w, &h) != 0 || w <= 0 || h <= 0) return false;

    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (!shot) return false;

    // Must run after draw() and before the present, while the target still
    // holds this frame's pixels.
    if (SDL_RenderReadPixels(sdl_, nullptr, SDL_PIXELFORMAT_ARGB8888,
                             shot->pixels, shot->pitch) != 0) {
        SDL_FreeSurface(shot);
        return false;
    }

    const bool ok = Image::SavePng(shot, path);
    SDL_FreeSurface(shot);
    return ok;
}

void App::saveScreenshot()
{
#ifdef __WIIU__
    // SDL_RenderReadPixels reads the tiled GX2 buffer as linear and returns
    // rubbish. Aroma's screenshot plugin untiles properly.
    toast("Use the screenshot plugin (writes to sd:/wiiu/screenshots)");
    LOGF("[shot] declined: GX2 readback is tiled, use Aroma's plugin");
    return;
#else
    const std::string dir = Platform::DataDir() + "/screenshots";
    Platform::MakeDirs(dir);

    // A counter, not a timestamp: the console's clock may not be set.
    char name[64];
    std::snprintf(name, sizeof(name), "/shot-%03d.png", ++screenshotCounter_);

    const bool ok = writeScreenshot(dir + name);
    toast(ok ? ("Saved " + std::string(name + 1)) : "Screenshot failed");
#endif
}

void App::setAutoScreenshot(const std::string& path, int delayMs)
{
    autoShotPath_ = path;
    autoShotAtMs_ = Platform::NowMs() + (uint64_t)std::max(0, delayMs);
}

void App::setScriptedInput(const std::vector<Action>& actions, int intervalMs)
{
    scriptedActions_  = actions;
    scriptIndex_      = 0;
    scriptIntervalMs_ = std::max(50, intervalMs);
    // Give the first library time to arrive before the replay starts.
    nextScriptedMs_   = Platform::NowMs() + (uint64_t)scriptIntervalMs_;

    // Never shoot before the script has finished.
    const uint64_t scriptEnds = nextScriptedMs_ +
        (uint64_t)scriptIntervalMs_ * (uint64_t)actions.size();
    if (autoShotAtMs_ < scriptEnds) autoShotAtMs_ = scriptEnds;
}

// ------------------------------------------------------------------ flow

void App::startup()
{
    screen_     = Screen::Connecting;
    statusLine_ = "Connecting to the network";

    pool_.submit([this] {
        const bool online = Http::Init();

        if (!online) {
            pool_.post([this] {
                errorLine_ = "No network connection.";
                statusLine_ = "Check the console's internet settings";
            });
            return;
        }

        // A stored session skips both discovery and sign-in entirely, which
        // is the common case after the first launch.
        const bool restored = client_.loadSession();
        LOGF("[auth] session %s", restored ? "restored" : "not found");

        pool_.post([this, restored] {
            if (restored) {
                statusLine_ = "Signed in as " + client_.userName();
                loadLibraries();
                return;
            }
            // Nothing set up at all: ask which service rather than assuming.
            screen_      = Screen::Welcome;
            welcomeRow_  = 0;
        });
    });
}

void App::beginDiscovery()
{
    screen_       = Screen::ServerSelect;
    statusLine_   = "Looking for Jellyfin servers";
    errorLine_.clear();
    servers_.clear();
    serverIndex_  = 0;
    discovering_  = true;

    pool_.submit([this] {
        std::vector<JfServer> found = JellyfinClient::Discover(2500);
        LOGF("[discovery] %lu server(s) answered", (unsigned long)found.size());

        // A saved server always appears, listed first, whether or not the
        // broadcast found anything.
        std::vector<JfServer> overrides = ReadServerOverrides();
        for (const JfServer& saved : overrides) {
            bool already = false;
            for (const JfServer& srv : found) {
                if (srv.address == saved.address) { already = true; break; }
            }
            if (!already) found.insert(found.begin(), saved);
            LOGF("[discovery] saved server %s", saved.address.c_str());
        }

        for (const JfServer& srv : found) {
            LOGF("[discovery]   %s at %s", srv.name.c_str(), srv.address.c_str());
        }
        pool_.post([this, found] {
            servers_     = found;
            serverIndex_ = 0;
            discovering_ = false;
            statusLine_  = found.empty() ? "No servers found on this network"
                                         : "Choose a server";
            LOGF("[discovery] offering %lu server(s) to the user",
                 (unsigned long)found.size());
        });
    });
}

void App::beginSignIn(const std::string& serverUrl)
{
    client_.setServerUrl(serverUrl);

    // Already signed into this one: pick the session back up instead of
    // asking for another Quick Connect code.
    if (client_.loadSessionFor(client_.serverUrl())) {
        LOGF("[auth] session for %s reused", client_.serverUrl().c_str());
        toast("Signed in as " + client_.userName());
        loadLibraries();
        return;
    }

    screen_           = Screen::SignIn;
    statusLine_       = "Contacting the server";
    errorLine_.clear();
    quickConnectCode_.clear();

    pool_.submit([this] {
        std::string serverName, error;
        if (!client_.ping(serverName, error)) {
            pool_.post([this, error] {
                errorLine_ = error;
                statusLine_ = "Could not reach that server";
            });
            return;
        }

        std::string code;
        if (!client_.quickConnectStart(code, error)) {
            pool_.post([this, error] {
                errorLine_  = error;
                statusLine_ = "Quick Connect unavailable";
            });
            return;
        }

        pool_.post([this, code, serverName] {
            quickConnectCode_  = code;
            signInServerName_  = serverName;
            statusLine_        = "Waiting for approval";
            nextPollMs_        = Platform::NowMs() + kPollIntervalMs;
            signInDeadlineMs_  = Platform::NowMs() + kSignInWindowMs;
        });
    });
}

void App::pollQuickConnect()
{
    if (pollInFlight_) return;
    pollInFlight_ = true;

    pool_.submit([this] {
        std::string error;
        const QuickConnectState state = client_.quickConnectPoll(error);

        if (state == QuickConnectState::Approved) {
            std::string finishError;
            const bool ok = client_.quickConnectFinish(finishError);
            pool_.post([this, ok, finishError] {
                pollInFlight_ = false;
                if (ok) {
                    toast("Signed in as " + client_.userName());
                    loadLibraries();
                } else {
                    errorLine_  = finishError;
                    statusLine_ = "Sign-in failed";
                }
            });
            return;
        }

        pool_.post([this, state] {
            pollInFlight_ = false;
            if (state == QuickConnectState::Expired) {
                quickConnectCode_.clear();
                statusLine_ = "That code expired";
                errorLine_  = "Press A to get a new one";
            }
        });
    });
}

void App::rebuildSidebar()
{
    sidebar_.clear();

    // Seerr has no libraries, so its sidebar is its own lists.
    if (activeSource_ == MediaSource::Seerr) {
        for (int i = 0; i < kDiscoverModeCount; ++i) {
            sidebar_.push_back({ SidebarEntry::Kind::Discover,
                                 kDiscoverModes[i].title, "", i });
        }
        for (int i = 0; i < kRequestFilterCount; ++i) {
            sidebar_.push_back({ SidebarEntry::Kind::Requests,
                                 kRequestFilters[i].title, "", i });
        }
        sidebar_.push_back({ SidebarEntry::Kind::Search, "Search", "", 0 });
        return;
    }

    sidebar_.push_back({ SidebarEntry::Kind::Home,   "Home",   "", 0 });
    sidebar_.push_back({ SidebarEntry::Kind::Search, "Search", "", 0 });
    for (const JfLibrary& library : libraries_) {
        sidebar_.push_back({ SidebarEntry::Kind::Library, library.name,
                             library.id, 0 });
    }
    // Settings and the service tabs are not in this list: they live in the
    // fixed bar at the bottom of the sidebar.
}

// The fixed bar at the bottom of the sidebar: one tab per service, then the
// gear. Dimmed until selected.
namespace {
struct ServiceTab { MediaSource source; const char* label; Color color; };
const ServiceTab kServiceTabs[] = {
    { MediaSource::Jellyfin, "Jellyfin", { 0x8B, 0x5C, 0xF6 } },
    { MediaSource::Seerr,    "Seerr",    { 0x4C, 0x9A, 0xFF } },
};
constexpr int kServiceTabCount = 2;

// The bar is two rows: which server, then which service.
constexpr int kServerRowHeight = 46;
constexpr int kTabRowHeight    = 64;
constexpr int kBottomBarHeight = kServerRowHeight + kTabRowHeight;
constexpr int kGearWidth       = 56;
}  // namespace

void App::switchSource(MediaSource source)
{
    if (source == activeSource_) return;
    // A tab with nothing behind it takes you to setting it up, rather than
    // to an empty library.
    if (source == MediaSource::Jellyfin && !client_.signedIn()) {
        bottomBarFocused_ = false;
        beginDiscovery();
        return;
    }
    if (source == MediaSource::Seerr && !seerr_.configured()) {
        if (!settings_.requestServerElsewhere && !seerrAutoTried_ &&
            client_.signedIn()) {
            toast("Looking for Seerr");
            tryRequestServerBesideJellyfin();
        } else {
            toast("No Seerr server: set one up in Settings");
        }
        return;
    }

    activeSource_ = source;
    showingSeerr_ = (source == MediaSource::Seerr);
    // Focus moves into the service rather than staying on the bar.
    bottomBarFocused_ = false;
    sidebarFocused_   = true;
    items_.clear();
    homeRows_.clear();
    navStack_.clear();
    itemIndex_ = 0;
    rebuildSidebar();
    libraryIndex_ = 0;
    if (showingSeerr_) {
        discoverMode_ = 0;
        loadDiscover();                  // sets Browse itself
    } else {
        screen_ = Screen::Home;
        loadHome();
    }
}

void App::openSidebarEntry(int index)
{
    if (index < 0 || index >= (int)sidebar_.size()) return;
    libraryIndex_ = index;

    switch (sidebar_[index].kind) {
        case SidebarEntry::Kind::Home:
            screen_       = Screen::Home;
            showingSeerr_ = false;
            homeRows_.clear();
            loadHome();
            break;
        case SidebarEntry::Kind::Discover:
            showingSeerr_ = true;
            discoverMode_ = sidebar_[index].mode;
            loadDiscover();              // sets Browse: a list is a grid
            break;
        case SidebarEntry::Kind::Requests:
            showingSeerr_ = true;
            loadRequests(sidebar_[index].mode);
            break;
        case SidebarEntry::Kind::Search:
            // Text entry starts on A, so passing over Search does not
            // capture the keyboard.
            screen_ = Screen::Browse;
            if (searchTerm_.empty()) {
                items_.clear();
                curTitle_ = "Search";
                navStack_.clear();
            }
            break;
        case SidebarEntry::Kind::Library:
            screen_ = Screen::Browse;
            loadLibraryItems(index);
            break;
        case SidebarEntry::Kind::Settings:
            screen_ = Screen::Settings;
            settingsRow_ = 1;   // 0 is the JELLYFIN heading
            break;
        case SidebarEntry::Kind::Services:
            // Landing on the row selects nothing: left and right pick a tab,
            // accept switches. See handleServiceTabs.
            break;
    }
}

// Seerr's lists, drawn by the same grid as everything else.
void App::resetPaging()
{
    pageLoader_ = nullptr;
    pageNext_   = 0;
    pageTotal_  = 0;
    pageLoading_ = false;
}

void App::appendPage(int requestId, const std::vector<JfItem>& fetched,
                     int nextPage, int total)
{
    if (requestId != itemsRequestId_) return;

    // Trending shifts between requests, so a title can arrive twice.
    for (const JfItem& item : fetched) {
        bool seen = false;
        for (const JfItem& have : items_) {
            if (have.id == item.id) { seen = true; break; }
        }
        if (!seen) items_.push_back(item);
    }

    pageNext_    = nextPage;
    pageTotal_   = total;
    pageLoading_ = false;
}

void App::maybeLoadMore()
{
    if (!pageLoader_ || pageNext_ <= 0 || pageLoading_) return;
    if (items_.empty()) return;

    // Two rows of headroom, so the fetch lands before the end does.
    const GridMetrics grid = gridMetrics();
    const int trigger = (int)items_.size() - grid.columns * 2;
    if (itemIndex_ < trigger) return;

    pageLoading_ = true;
    pageLoader_(pageNext_);
}

int App::resetGrid(LevelKind kind, const std::string& title)
{
    curKind_   = kind;
    curTitle_  = title;
    items_.clear();
    itemIndex_ = 0;
    gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
    itemsLoading_ = true;
    errorLine_.clear();
    return ++itemsRequestId_;
}

int App::beginGridLoad(const std::string& title)
{
    const int requestId = resetGrid(LevelKind::Library, title);
    curParentId_.clear();
    navStack_.clear();
    screen_ = Screen::Browse;
    resetPaging();
    return requestId;
}

void App::firstPage(int requestId, const std::vector<JfItem>& found,
                    const std::string& error, int nextPage, int total)
{
    if (requestId != itemsRequestId_) return;
    items_ = found;
    itemIndex_ = 0;
    gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
    itemsLoading_ = false;
    pageNext_  = nextPage;
    pageTotal_ = total;
    errorLine_ = found.empty() ? error : std::string();
}

void App::loadRequests(int filter)
{
    if (filter < 0 || filter >= kRequestFilterCount) filter = 0;

    const int requestId = beginGridLoad(kRequestFilters[filter].title);
    const std::string which = kRequestFilters[filter].filter;

    pageLoader_ = [this, which, requestId](int page) {
        pool_.submit([this, which, requestId, page] {
            std::string error;
            SeerrClient::Page info;
            std::vector<JfItem> found =
                seerr_.requests(which.c_str(), page, BJ_REQUEST_LIST, error, info);
            const int next = info.more() ? info.page + 1 : 0;
            const int total = info.totalResults;
            pool_.post([this, requestId, found, next, total] {
                appendPage(requestId, found, next, total);
            });
        });
    };

    pool_.submit([this, which, requestId] {
        std::string error;
        SeerrClient::Page info;
        std::vector<JfItem> found =
            seerr_.requests(which.c_str(), 1, BJ_REQUEST_LIST, error, info);
        const int next = info.more() ? 2 : 0;
        const int total = info.totalResults;
        pool_.post([this, found, error, requestId, next, total] {
            firstPage(requestId, found, error, next, total);
        });
    });
}

void App::loadDiscover()
{
    if (discoverMode_ < 0 || discoverMode_ >= kDiscoverModeCount) discoverMode_ = 0;
    const DiscoverMode& mode = kDiscoverModes[discoverMode_];

    // Film and show genres have different ids.
    const std::string media = mode.media;
    if (discoverGenresFor_ != media) {
        discoverGenre_.clear();
        discoverGenreName_.clear();
        discoverGenresFor_ = media;
        discoverGenres_.clear();
    }

    std::string path = mode.path;
    if (!discoverGenre_.empty() && mode.media[0]) {
        path += (path.find('?') == std::string::npos ? "?" : "&");
        path += "genre=" + discoverGenre_;
    }

    const int requestId = beginGridLoad(
        discoverGenreName_.empty() ? std::string(mode.title)
                                   : mode.title + (": " + discoverGenreName_));

    pageLoader_ = [this, path, requestId](int page) {
        pool_.submit([this, path, requestId, page] {
            std::string error;
            SeerrClient::Page info;
            std::vector<JfItem> chunk =
                seerr_.discover(path.c_str(), page, error, info);

            std::vector<JfItem> found;
            for (const JfItem& item : chunk) {
                if (!item.artUrl.empty()) found.push_back(item);
            }
            const int next = info.more() ? info.page + 1 : 0;
            pool_.post([this, requestId, found, next] {
                appendPage(requestId, found, next, 0);
            });
        });
    };

    pool_.submit([this, path, requestId] {
        std::string error;
        SeerrClient::Page info;
        std::vector<JfItem> chunk = seerr_.discover(path.c_str(), 1, error, info);

        std::vector<JfItem> found;
        for (const JfItem& item : chunk) {
            if (!item.artUrl.empty()) found.push_back(item);
        }
        LOGF("[seerr] %s: %lu of %d", path.c_str(),
             (unsigned long)found.size(), info.totalResults);
        const int next = info.more() ? 2 : 0;
        // Filtered client side, so the server's total is not reachable.
        const int total = 0;
        pool_.post([this, found, error, requestId, next, total] {
            firstPage(requestId, found, error, next, total);
        });
    });

    // Fetched once per media type.
    if (!media.empty() && discoverGenres_.empty()) {
        pool_.submit([this, media] {
            std::string error;
            std::vector<JfItem> found = seerr_.genres(media.c_str(), error);
            pool_.post([this, media, found] {
                if (discoverGenresFor_ != media) return;
                discoverGenres_ = found;
            });
        });
    }
}

// What the interface lays itself out to.
void App::rememberWindowSize()
{
    if (!window_) return;
    // Only the restored size, not a maximized or fullscreen one.
    if (SDL_GetWindowFlags(window_) &
        (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) {
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w < 640 || h < 480) return;
    if (w == settings_.windowWidth && h == settings_.windowHeight) return;
    settings_.windowWidth  = w;
    settings_.windowHeight = h;
    settings_.save();
}

void App::updateLayoutSize()
{
    // Zero without a logical size, which is the case off the Wii U.
    SDL_RenderGetLogicalSize(sdl_, &logicalW_, &logicalH_);
    if (logicalW_ <= 0 || logicalH_ <= 0) {
        SDL_GetRendererOutputSize(sdl_, &logicalW_, &logicalH_);
    }
    if (logicalW_ <= 0 || logicalH_ <= 0) {
        logicalW_ = 1280;
        logicalH_ = 720;
    }
}

// Fetched together, so the home screen arrives complete.
void App::loadHome()
{
    if (homeLoading_) return;
    homeLoading_ = true;
    homeRows_.clear();
    homeRow_ = 0;

    pool_.submit([this] {
        std::string error;
        std::vector<HomeRow> rows;

        std::vector<JfItem> resume = client_.resumable(error);
        if (!resume.empty()) rows.push_back({ "Continue Watching", resume, 0, 0 });

        std::vector<JfItem> next = client_.nextUp(error);
        if (!next.empty()) rows.push_back({ "Next up", next, 0, 0 });

        std::vector<JfItem> latest = client_.recentlyAdded(error);
        if (!latest.empty()) rows.push_back({ "Recently added", latest, 0, 0 });

        std::vector<JfItem> loved = client_.favorites(error);
        if (!loved.empty()) rows.push_back({ "Favorites", loved, 0, 0 });

        LOGF("[home] %lu row(s)", (unsigned long)rows.size());

        pool_.post([this, rows] {
            homeRows_ = rows;
            homeRow_  = 0;
            homeLoading_ = false;
        });
    });
}

void App::loadLibraries()
{
    screen_     = Screen::Browse;
    statusLine_ = "Loading libraries";
    errorLine_.clear();

    // Seerr is paired to the library, so a different one brings its own.
    if (seerrPairedWith_ != client_.serverUrl()) {
        seerrPairedWith_ = client_.serverUrl();
        seerr_.pairWith(seerrPairedWith_);
        seerrAutoTried_ = false;
    }

    pool_.submit([this] {
        std::string error;
        std::vector<JfLibrary> libs;

        // macOS fails the request that raises its local network prompt.
        for (int attempt = 0; attempt < 4; ++attempt) {
            error.clear();
            libs = client_.libraries(error);
            if (!libs.empty() || error.empty()) break;
            LOGF("[library] attempt %d: %s", attempt + 1, error.c_str());
            bj::SleepMs(1500);
        }
        LOGF("[library] %lu librarie(s)%s%s", (unsigned long)libs.size(),
             error.empty() ? "" : " - ", error.c_str());
        pool_.post([this, libs, error] {
            libraries_    = libs;
            rebuildSidebar();
            libraryIndex_ = 0;
            if (libs.empty()) {
                errorLine_  = error.empty() ? "This account has no libraries" : error;
                statusLine_ = "Nothing to show";
                return;
            }
            statusLine_.clear();
            tryRequestServerBesideJellyfin();
            // Home, not the first library: that is where a half-watched
            // episode is.
            openSidebarEntry(0);
        });
    });
}

namespace {

const char* kBrowseMenuTitles[] = { "Sort", "Order", "Show", "Genre" };

// What each entry under Show asks the server for.
struct ShowOption { const char* label; const char* filters; int isFavorite; };
const ShowOption kShowOptions[] = {
    { "Everything",       "",             -1 },
    { "Not watched",      "IsUnplayed",   -1 },
    { "Watched",          "IsPlayed",     -1 },
    { "Favorites",        "",              1 },
    { "Started",          "IsResumable",  -1 },
};

}  // namespace

std::string App::browseCacheKey(const std::string& libraryId) const
{
    int sortCount = 0;
    const JfSortOption* sorts = JfSortOptions(sortCount);
    const int sortIndex = (browseSort_ >= 0 && browseSort_ < sortCount) ? browseSort_ : 0;

    return libraryId + "|" + sorts[sortIndex].sortBy +
           (browseDescending_ ? "|desc" : "|asc") + "|" +
           bj::ToString(browseShow_) + "|" + browseGenre_;
}

void App::applyBrowseFilters(JfQuery& query) const
{
    int sortCount = 0;
    const JfSortOption* sorts = JfSortOptions(sortCount);
    const int sortIndex = (browseSort_ >= 0 && browseSort_ < sortCount) ? browseSort_ : 0;
    query.sortBy    = sorts[sortIndex].sortBy;
    query.sortOrder = browseDescending_ ? "Descending" : "Ascending";

    const int showCount = (int)(sizeof(kShowOptions) / sizeof(kShowOptions[0]));
    if (browseShow_ > 0 && browseShow_ < showCount) {
        query.filters    = kShowOptions[browseShow_].filters;
        query.isFavorite = kShowOptions[browseShow_].isFavorite;
    }
    if (!browseGenre_.empty()) {
        query.genres    = browseGenre_;
        query.recursive = true;
    }
    if (!query.filters.empty() || query.isFavorite >= 0) query.recursive = true;
}

void App::loadBrowseFilterOptions(const std::string& libraryId)
{
    if (libraryId.empty() || browseFiltersFor_ == libraryId) return;
    browseFiltersFor_ = libraryId;
    browseFilters_ = JfFilterOptions();

    pool_.submit([this, libraryId] {
        JfFilterOptions options;
        std::string error;
        if (!client_.filterOptions(libraryId, options, error)) return;
        pool_.post([this, options, libraryId] {
            if (browseFiltersFor_ != libraryId) return;
            browseFilters_ = options;
        });
    });
}

int App::browseMenuOptionCount(int tab) const
{
    // The lists are in the sidebar; only a genre is chosen here.
    if (activeSource_ == MediaSource::Seerr) {
        (void)tab;
        return (int)discoverGenres_.size() + 1;
    }
    switch (tab) {
        case 0: { int count = 0; JfSortOptions(count); return count; }
        case 1: return 2;
        case 2: return (int)(sizeof(kShowOptions) / sizeof(kShowOptions[0]));
        case 3: return (int)browseFilters_.genres.size() + 1;
        default: return 0;
    }
}

void App::applyBrowseMenuChoice(int tab, int row)
{
    if (activeSource_ == MediaSource::Seerr) {
        (void)tab;
        if (row == 0) {
            discoverGenre_.clear();
            discoverGenreName_.clear();
        } else {
            const int which = row - 1;
            if (which < 0 || which >= (int)discoverGenres_.size()) return;
            // The id is a discover path: the fragment after "genre=".
            std::string path;
            ParseSeerrPathId(discoverGenres_[(size_t)which].id, path);
            const size_t equals = path.rfind("genre=");
            discoverGenre_ = (equals == std::string::npos)
                           ? std::string() : path.substr(equals + 6);
            discoverGenreName_ = discoverGenres_[(size_t)which].name;
        }
        loadDiscover();
        return;
    }

    int sortCount = 0;
    const JfSortOption* sorts = JfSortOptions(sortCount);

    switch (tab) {
        case 0:
            if (row < 0 || row >= sortCount) return;
            browseSort_ = row;
            browseDescending_ = sorts[row].descendingByDefault;
            break;
        case 1:
            browseDescending_ = (row == 1);
            break;
        case 2:
            browseShow_ = row;
            break;
        case 3:
            browseGenre_ = (row == 0) ? std::string()
                                      : browseFilters_.genres[(size_t)(row - 1)];
            break;
        default:
            return;
    }
    loadLibraryItems(libraryIndex_);
}

void App::handleBrowseMenuAction(Action action)
{
    const int tabs = (activeSource_ == MediaSource::Seerr) ? 1 : 4;
    const int count = browseMenuOptionCount(browseMenuTab_);

    switch (action) {
        case Action::Back:
        case Action::Options:
            browseMenuOpen_ = false;
            break;
        case Action::Left:
            if (browseMenuTab_ > 0) { --browseMenuTab_; browseMenuRow_ = 0; }
            break;
        case Action::Right:
            if (browseMenuTab_ + 1 < tabs) { ++browseMenuTab_; browseMenuRow_ = 0; }
            break;
        case Action::Up:
            if (browseMenuRow_ > 0) --browseMenuRow_;
            break;
        case Action::Down:
            if (browseMenuRow_ + 1 < count) ++browseMenuRow_;
            break;
        case Action::Accept:
            applyBrowseMenuChoice(browseMenuTab_, browseMenuRow_);
            browseMenuOpen_ = false;
            break;
        default:
            break;
    }
}

void App::drawBrowseMenu()
{
    const bool discovering = (activeSource_ == MediaSource::Seerr);
    const char* const kDiscoverTitles[] = { "Genre" };
    const int tabCount = discovering ? 1 : 4;

    const int panelW = 400;
    const int panelX = logicalW_ - panelW - 40;
    const int panelY = kTopBarHeight + 20;
    const int panelH = logicalH_ - panelY - 90;
    render_.fillRoundedRect({ panelX, panelY, panelW, panelH }, 12, Palette::Panel);

    int x = panelX + 20;
    for (int i = 0; i < tabCount; ++i) {
        const bool active = (i == browseMenuTab_);
        const std::string title = discovering ? kDiscoverTitles[i]
                                              : kBrowseMenuTitles[i];
        const int width = render_.textWidth(title, FontSize::Body) + 22;
        if (active) {
            render_.fillRoundedRect({ x - 10, panelY + 14, width, 34 }, 8,
                                    Palette::Accent);
        }
        render_.drawText(title, x, panelY + 20, FontSize::Body,
                         active ? Palette::Text : Palette::TextDim);
        x += width + 6;
    }

    const int rowHeight = 38;
    const int listTop = panelY + 66;
    const int visible = (panelH - 76) / rowHeight;
    const int count = browseMenuOptionCount(browseMenuTab_);
    int first = 0;
    if (browseMenuRow_ >= visible) first = browseMenuRow_ - visible + 1;

    int sortCount = 0;
    const JfSortOption* sorts = JfSortOptions(sortCount);

    for (int i = first; i < count && i - first < visible; ++i) {
        const int y = listTop + (i - first) * rowHeight;
        const bool focused = (i == browseMenuRow_);
        if (focused) {
            render_.fillRoundedRect({ panelX + 10, y - 4, panelW - 20, rowHeight - 4 },
                                    8, Palette::PanelHi);
        }

        std::string label;
        bool current = false;
        if (discovering) {
            if (i == 0) {
                label   = "Any genre";
                current = discoverGenreName_.empty();
            } else {
                label   = discoverGenres_[(size_t)(i - 1)].name;
                current = (label == discoverGenreName_);
            }
            render_.drawTextClipped(label, panelX + 24, y, panelW - 70,
                                    FontSize::Body,
                                    focused ? Palette::Text : Palette::TextDim);
            if (current) {
                render_.fillRoundedRect({ panelX + panelW - 34, y + 8, 10, 10 }, 5,
                                        Palette::Accent);
            }
            continue;
        }
        switch (browseMenuTab_) {
            case 0:
                label   = sorts[i].label;
                current = (i == browseSort_);
                break;
            case 1:
                label   = (i == 0) ? "Ascending" : "Descending";
                current = (browseDescending_ == (i == 1));
                break;
            case 2:
                label   = kShowOptions[i].label;
                current = (i == browseShow_);
                break;
            default:
                if (i == 0) {
                    label   = "Any genre";
                    current = browseGenre_.empty();
                } else {
                    label   = browseFilters_.genres[(size_t)(i - 1)];
                    current = (label == browseGenre_);
                }
                break;
        }

        render_.drawTextClipped(label, panelX + 24, y, panelW - 70, FontSize::Body,
                                focused ? Palette::Text : Palette::TextDim);
        if (current) {
            render_.fillRoundedRect({ panelX + panelW - 34, y + 8, 10, 10 }, 5,
                                    Palette::Accent);
        }
    }
}

void App::loadLibraryItems(int sidebarIndex)
{
    if (sidebarIndex < 0 || sidebarIndex >= (int)sidebar_.size()) return;
    if (sidebar_[sidebarIndex].kind != SidebarEntry::Kind::Library) return;

    libraryIndex_ = sidebarIndex;
    items_.clear();
    itemIndex_    = 0;
    gridScrollPx_ = 0.0f;
    gridScrollTargetPx_ = 0.0f;
    itemsLoading_ = true;

    // Switching libraries starts a fresh navigation path.
    navStack_.clear();
    curKind_     = LevelKind::Library;
    curTitle_    = sidebar_[sidebarIndex].title;
    curParentId_ = sidebar_[sidebarIndex].libraryId;
    curSeriesId_.clear();

    JfLibrary library;
    library.id   = sidebar_[sidebarIndex].libraryId;
    library.name = sidebar_[sidebarIndex].title;
    for (const JfLibrary& candidate : libraries_) {
        if (candidate.id == library.id) { library = candidate; break; }
    }
    const int requestId = ++itemsRequestId_;
    loadBrowseFilterOptions(library.id);
    const std::string cacheKey = browseCacheKey(library.id);

    // Built here: the menu can change while a fetch is in flight.
    JfQuery query;
    query.parentId         = library.id;
    query.includeItemTypes = DefaultItemTypesFor(library.collectionType);
    query.limit            = kLibraryPageSize;
    applyBrowseFilters(query);

    // Before the cache is consulted: a cached listing still pages.
    resetPaging();
    pageLoader_ = [this, query, requestId](int page) {
        JfQuery next = query;
        next.startIndex = (page - 1) * kLibraryPageSize;
        pool_.submit([this, next, requestId, page] {
            std::string error;
            int total = 0;
            std::vector<JfItem> fetched = client_.items(next, error, total);
            const int following =
                (next.startIndex + (int)fetched.size() < total && !fetched.empty())
                    ? page + 1 : 0;
            pool_.post([this, requestId, fetched, following, total] {
                appendPage(requestId, fetched, following, total);
            });
        });
    };

    std::map<std::string, CachedListing>::const_iterator hit =
        itemsCache_.find(cacheKey);
    if (hit != itemsCache_.end()) {
        items_        = hit->second.items;
        itemsLoading_ = false;
        errorLine_.clear();
        pageNext_     = hit->second.nextPage;
        pageTotal_    = hit->second.total;
        return;
    }

    pool_.submit([this, library, requestId, cacheKey, query] {
        std::string error;
        int total = 0;
        std::vector<JfItem> fetched = client_.items(query, error, total);

        LOGF("[library] %s: %lu item(s)%s%s", library.name.c_str(),
             (unsigned long)fetched.size(),
             error.empty() ? "" : " - ", error.c_str());
        const int next = ((int)fetched.size() < total && !fetched.empty()) ? 2 : 0;
        pool_.post([this, fetched, error, requestId, cacheKey, next, total] {
            // The user moved on before this landed; drop it.
            if (requestId != itemsRequestId_) return;

            items_        = fetched;
            itemIndex_    = 0;
            gridScrollPx_ = 0.0f;
            gridScrollTargetPx_ = 0.0f;
            itemsLoading_ = false;
            pageNext_     = next;
            pageTotal_    = total;
            if (fetched.empty() && !error.empty()) {
                errorLine_ = error;
            } else {
                errorLine_.clear();
                if (!fetched.empty()) {
                    CachedListing entry;
                    entry.items    = fetched;
                    entry.nextPage = next;
                    entry.total    = total;
                    itemsCache_[cacheKey] = entry;
                }
            }
        });
    });
}

void App::pushCurrentLevel()
{
    BrowseLevel level;
    level.kind           = curKind_;
    level.title          = curTitle_;
    level.parentId       = curParentId_;
    level.seriesId       = curSeriesId_;
    level.items          = items_;
    level.itemIndex      = itemIndex_;
    level.scrollPx       = gridScrollPx_;
    level.scrollTargetPx = gridScrollTargetPx_;
    navStack_.push_back(std::move(level));
}

bool App::popLevel()
{
    if (navStack_.empty()) return false;

    BrowseLevel level = std::move(navStack_.back());
    navStack_.pop_back();

    curKind_     = level.kind;
    curTitle_    = level.title;
    curParentId_ = level.parentId;
    curSeriesId_ = level.seriesId;
    items_       = std::move(level.items);
    itemIndex_   = level.itemIndex;
    // Restore the scroll exactly, so coming back does not re-animate.
    gridScrollPx_       = level.scrollPx;
    gridScrollTargetPx_ = level.scrollTargetPx;
    itemsLoading_ = false;
    errorLine_.clear();
    // Nothing in flight for this level matters any more.
    ++itemsRequestId_;
    return true;
}

std::string App::breadcrumb() const
{
    std::string path;
    for (const BrowseLevel& level : navStack_) {
        if (level.kind == LevelKind::Library) continue;   // the sidebar shows it
        path += level.title + "   >   ";
    }
    if (curKind_ != LevelKind::Library) path += curTitle_;
    return path;
}

// Decides what pressing A on a tile means: drill in, or open the detail page.
void App::enterItem(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= (int)items_.size()) return;
    const JfItem item = items_[itemIndex];

    // A Seerr result has no seasons, and its id means nothing to Jellyfin.
    if (item.source == MediaSource::Seerr && item.type != "Genre") {
        openDetail(itemIndex);
        return;
    }

    if (item.type == "Series") {
        pushCurrentLevel();
        loadSeasons(item.id, item.name);
        return;
    }
    if (item.type == "Season") {
        pushCurrentLevel();
        // Episodes come from the series endpoint, so carry the series id down.
        loadEpisodes(curSeriesId_.empty() ? curParentId_ : curSeriesId_,
                     item.id, item.name);
        return;
    }
    // A genre or another list Seerr builds.
    std::string discoverPath;
    if (ParseSeerrPathId(item.id, discoverPath)) {
        pushCurrentLevel();
        curKind_     = LevelKind::Folder;
        curTitle_    = item.name;
        curParentId_ = item.id;
        items_.clear();
        itemIndex_ = 0;
        gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
        itemsLoading_ = true;
        const int requestId = ++itemsRequestId_;
        pool_.submit([this, discoverPath, requestId] {
            std::string error;
            std::vector<JfItem> found = seerr_.discover(discoverPath.c_str(), 1, error);
            pool_.post([this, found, error, requestId] {
                if (requestId != itemsRequestId_) return;
                items_ = found;
                itemIndex_ = 0;
                gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
                itemsLoading_ = false;
                errorLine_ = found.empty() ? error : std::string();
            });
        });
        return;
    }

    // A collection or an album: whatever is inside it, in its own order.
    if (item.isFolder && item.source == MediaSource::Jellyfin) {
        pushCurrentLevel();
        loadFolder(item.id, item.name);
        return;
    }
    openDetail(itemIndex);
}

void App::loadFolder(const std::string& folderId, const std::string& title)
{
    const int requestId = beginGridLoad(title);
    curKind_     = LevelKind::Folder;
    curParentId_ = folderId;

    pool_.submit([this, folderId, requestId] {
        std::string error;
        int total = 0;
        JfQuery q;
        q.parentId = folderId;
        q.sortBy   = "SortName";
        q.limit    = kLibraryPageSize;
        std::vector<JfItem> fetched = client_.items(q, error, total);
        const int next = ((int)fetched.size() < total && !fetched.empty()) ? 2 : 0;
        pool_.post([this, fetched, error, requestId, next, total] {
            firstPage(requestId, fetched, error, next, total);
        });
    });
}

void App::loadSeasons(const std::string& seriesId, const std::string& title)
{
    const int requestId = resetGrid(LevelKind::Seasons, title);
    curParentId_ = seriesId;
    curSeriesId_ = seriesId;

    pool_.submit([this, seriesId, requestId] {
        std::string error;
        std::vector<JfItem> fetched = client_.seasons(seriesId, error);
        if (fetched.empty() && error.empty()) error = "No seasons here";
        pool_.post([this, fetched, error, requestId] {
            firstPage(requestId, fetched, error, 0, 0);
        });
    });
}

void App::loadEpisodes(const std::string& seriesId, const std::string& seasonId,
                       const std::string& title)
{
    const int requestId = resetGrid(LevelKind::Episodes, title);
    curParentId_ = seasonId;
    curSeriesId_ = seriesId;

    pool_.submit([this, seriesId, seasonId, requestId] {
        std::string error;
        std::vector<JfItem> fetched = client_.episodes(seriesId, seasonId, error);
        if (fetched.empty() && error.empty()) error = "No episodes here";
        pool_.post([this, fetched, error, requestId] {
            firstPage(requestId, fetched, error, 0, 0);
        });
    });
}

void App::openDetail(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= (int)items_.size()) return;
    detailItem_ = items_[itemIndex];
    screen_     = Screen::Detail;

    // The grid query omits Overview, so fetch the full record now.
    const std::string itemId = detailItem_.id;

    // A single title carries genres, runtime and cast; a listing does not.
    std::string mediaType;
    long long tmdbId = 0;
    if (detailItem_.source == MediaSource::Seerr &&
        ParseSeerrItemId(itemId, mediaType, tmdbId)) {
        pool_.submit([this, itemId, mediaType, tmdbId] {
            JfItem full;
            std::string error;
            if (!seerr_.title(mediaType, tmdbId, full, error)) return;
            pool_.post([this, full, itemId] {
                if (screen_ == Screen::Detail && detailItem_.id == itemId) {
                    detailItem_ = full;
                }
            });
        });
        return;
    }

    pool_.submit([this, itemId] {
        JfItem full;
        std::string error;
        if (!client_.itemDetailsFull(itemId, full, error)) return;
        pool_.post([this, full, itemId] {
            if (screen_ == Screen::Detail && detailItem_.id == itemId) {
                detailItem_ = full;
            }
        });
    });
}

void App::startPlayback(const JfItem& item, bool fromStart)
{
    const bool sameItem = (item.id == playingItem_.id);
    stopPlayback();

    // A seek or track change keeps the selection; a new film resets it.
    if (!sameItem) {
        playSourceIndex_   = 0;
        playAudioIndex_    = -1;
        playSubtitleIndex_ = -1;
        subtitleCues_.clear();
        subtitleCueHint_ = 0;
        ++subtitleRequestId_;
    }
    playerMenuOpen_ = false;
    playerMenuTab_  = 0;
    playerMenuRow_  = 0;

    playingItem_ = item;
    player_.reset(new Player());

    // Pick up where the server says this was left, unless asked not to.
    const double startSeconds = fromStart ? 0.0 : (double)item.resumeSeconds();
    if (startSeconds > 0.0) {
        LOGF("[player] resuming at %d seconds", (int)startSeconds);
    }

    std::string error;
    // 0 on either side means "no ceiling", so it cannot win a min().
    int height = settings_.playbackHeight;
    if (kPlaybackMaxHeight > 0 && (height <= 0 || height > kPlaybackMaxHeight)) {
        height = kPlaybackMaxHeight;
    }
    JellyfinClient::PlaybackRequest request;
    request.audioIndex = playAudioIndex_;
    // Text tracks are drawn here; only bitmap ones are burned in.
    request.subtitleIndex = -2;
    if (playSubtitleIndex_ >= 0) {
        const JfMediaSource* source = playPlan_.source();
        const JfStream* stream = source ? source->streamAt(playSubtitleIndex_) : nullptr;
        if (stream && !stream->isText) request.subtitleIndex = playSubtitleIndex_;
    }
    if (playSourceIndex_ > 0 && playSourceIndex_ < (int)playPlan_.sources.size()) {
        request.mediaSourceId = playPlan_.sources[(size_t)playSourceIndex_].id;
    }
    if (!player_->open(client_, item.id, height, startSeconds, request, error)) {
        toast(error.empty() ? "Could not start playback" : error);
        player_.reset();
        return;
    }

    screen_ = Screen::Playing;
    endHandled_ = false;
    controlsUntilMs_ = Platform::NowMs() + 3000;
    statFrames_ = 0;
    statConvertMs_ = statUploadMs_ = 0.0;
    // The loop counters run the whole time the app is up.
    statLoops_ = 0;
    statDrawMs_ = statPresentSumMs_ = 0.0;
    statLastLogMs_ = 0;
    lastProgressReportMs_ = Platform::NowMs();
    client_.reportPlaybackStart(item.id);
    LOGF("[player] opening %s at up to %dp", item.name.c_str(), height);
#if defined(_XBOX) && !defined(_XENON)
    Platform::LogMemory("before playback");
#endif
}

void App::stopPlayback()
{
    if (!player_) return;

    // Absolute position in the film: a resumed stream starts partway in.
    const double position = player_->startOffsetSeconds() + player_->positionSeconds();
    client_.reportPlaybackStopped(playingItem_.id, (int64_t)(position * 10000000.0));

    player_->close();
    player_.reset();

    for (SDL_Texture*& texture : videoTextures_) {
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
    }
    videoTexture_ = nullptr;
    videoTextureIndex_ = 0;
    videoWidth_ = videoHeight_ = 0;
    haveVideoFrame_ = false;
}

// A seek restarts the stream, so five presses should cost one request.
void App::seekBy(int deltaSeconds)
{
    if (!player_) return;

    const double current = (seekTargetSeconds_ >= 0.0)
        ? seekTargetSeconds_
        : player_->startOffsetSeconds() + player_->positionSeconds();

    double target = current + deltaSeconds;
    if (target < 0.0) target = 0.0;

    const double duration = playingItem_.runtimeSeconds();
    if (duration > 0.0 && target > duration - 5.0) target = duration - 5.0;

    seekTargetSeconds_ = target;
    seekApplyAtMs_     = Platform::NowMs() + 700;
    controlsUntilMs_   = Platform::NowMs() + 4000;
}

void App::applyPendingSeek()
{
    if (seekTargetSeconds_ < 0.0) return;
    if (Platform::NowMs() < seekApplyAtMs_) return;

    JfItem target = playingItem_;
    target.resumeTicks = (int64_t)(seekTargetSeconds_ * 10000000.0);
    seekTargetSeconds_ = -1.0;

    LOGF("[player] seeking to %d seconds",
         (int)(target.resumeTicks / 10000000));
    startPlayback(target, false);
}

// The end of an episode leads into the next one.
void App::handlePlaybackEnded()
{
    if (endHandled_) return;
    endHandled_ = true;

    const JfItem finished = playingItem_;

    // Whatever happens next, this one has been watched.
    const std::string finishedId = finished.id;
    pool_.submit([this, finishedId] { client_.markPlayed(finishedId, true); });
    detailItem_.played = true;

    if (finished.seriesId.empty()) {
        // A film: back to where it was started from.
        stopPlayback();
        screen_ = Screen::Detail;
        return;
    }

    toast("Looking for the next episode");
    const std::string seriesId = finished.seriesId;
    const std::string seasonId = finished.seasonId;

    pool_.submit([this, seriesId, seasonId, finishedId] {
        std::string error;
        std::vector<JfItem> episodes = client_.episodes(seriesId, seasonId, error);

        JfItem next;
        for (size_t i = 0; i + 1 < episodes.size(); ++i) {
            if (episodes[i].id == finishedId) { next = episodes[i + 1]; break; }
        }

        pool_.post([this, next] {
            if (next.id.empty()) {
                // End of the season, or the list did not come back.
                toast("That was the last episode");
                stopPlayback();
                screen_ = Screen::Detail;
                return;
            }
            LOGF("[player] continuing with %s", next.name.c_str());
            detailItem_ = next;
            startPlayback(next, true);
        });
    });
}

void App::handlePlayerAction(Action action)
{
    controlsUntilMs_ = Platform::NowMs() + 4000;

    if (playerMenuOpen_) { handlePlayerMenuAction(action); return; }

    switch (action) {
        case Action::Options:
            if (!playPlan_.sources.empty()) {
                playerMenuOpen_ = true;
                playerMenuTab_  = 0;
                playerMenuRow_  = 0;
            }
            return;
        case Action::Back:
            stopPlayback();
            screen_ = Screen::Detail;
            break;
        case Action::Accept:
            if (player_) player_->setPaused(!player_->paused());
            break;
        case Action::Left:      seekBy(-10);  break;
        case Action::Right:     seekBy(10);   break;
        case Action::PageUp:    seekBy(-60);  break;
        case Action::PageDown:  seekBy(60);   break;
        case Action::Up:        seekBy(300);  break;
        case Action::Down:      seekBy(-300); break;
        default:
            break;
    }
}

bool App::updateVideoTexture()
{
    if (!player_) return false;

    if (!player_->nextFrame(currentVideoFrame_)) return false;
    if (!currentVideoFrame_.valid()) return false;
    haveVideoFrame_ = true;

    const Nv12Frame& frame = currentVideoFrame_;

#ifdef __WIIU__
    // The GPU samples this frame during drawPlayer, so there is nothing to
    // convert or upload here.
    if (gx2Ready_) {
        ++statFrames_;
        const uint64_t now = Platform::NowMs();
        if (statLastLogMs_ == 0) statLastLogMs_ = now;
        if (settings_.diagnostics && now - statLastLogMs_ >= 2000 && statFrames_ > 0) {
            const int decMs = player_ ? player_->takeDecodeMs() : 0;
            const int decFr = player_ ? player_->takeDecodeFrames() : 0;
            LOGF("[player] %d frames in %lums: decode %s copy %s flush %s draw %s "
                 "present %s, queue %d, segments %d, decoded %d, dropped %d",
                 statFrames_, (unsigned long)(now - statLastLogMs_),
                 bj::ToString(decFr > 0 ? (double)decMs / decFr : 0.0, 1).c_str(),
                 bj::ToString(gx2Video_.lastCopyMs(), 1).c_str(),
                 bj::ToString(gx2Video_.lastFlushMs(), 1).c_str(),
                 bj::ToString(gx2Video_.lastDrawMs(), 1).c_str(),
                 bj::ToString(statPresentMs_, 1).c_str(),
                 player_->queuedFrames(), player_->bufferedSegments(),
                 player_->decodedFrames(), player_->droppedFrames());
            statFrames_ = 0;
            statLastLogMs_ = now;
        }
        return true;
    }
#endif

    // Only when the stream's size changes, which is once per item.
    if (frame.width != videoWidth_ || frame.height != videoHeight_ ||
        !videoTextures_[0]) {
        for (SDL_Texture*& texture : videoTextures_) {
            if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        }
        videoUploadsYuv_ = kPreferYuvUpload && RendererTakesYuv(sdl_);
        const Uint32 format = videoUploadsYuv_ ? kVideoYuvFormat : kVideoRgbFormat;

        for (SDL_Texture*& texture : videoTextures_) {
            // A format the renderer advertises. Any other and SDL keeps a
            // shadow texture and converts on unlock: 88ms a frame, not 3ms.
            texture = SDL_CreateTexture(sdl_, format,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        frame.width, frame.height);
            if (!texture) {
                LOGF("[player] could not create a %dx%d texture: %s",
                     frame.width, frame.height, SDL_GetError());
                return false;
            }
        }
        videoTextureIndex_ = 0;
        videoWidth_  = frame.width;
        videoHeight_ = frame.height;

        // From SDL's own masks, not an assumption about endianness.
        int bpp = 0;
        Uint32 rMask = 0, gMask = 0, bMask = 0, aMask = 0;
        if (!SDL_ISPIXELFORMAT_FOURCC(format) &&
            SDL_PixelFormatEnumToMasks(format, &bpp, &rMask, &gMask, &bMask, &aMask)) {
            videoBytes_ = PixelBytesFromMasks(rMask, gMask, bMask, aMask);
            LOGF("[player] pixel bytes r=%d g=%d b=%d a=%d",
                 videoBytes_.r, videoBytes_.g, videoBytes_.b, videoBytes_.a);
        }
        LOGF("[player] %d video textures of %dx%d, %s",
             kVideoTextureCount, frame.width, frame.height,
             videoUploadsYuv_ ? "YUV planes, GPU converts"
#if defined(BJ_VIDEO_YUY2)
                              : "YUY2, GPU converts");
#else
                              : "RGB, CPU converts");
#endif
    }

    // Write to the next texture in the rotation, not the one on screen.
    videoTextureIndex_ = (videoTextureIndex_ + 1) % kVideoTextureCount;
    SDL_Texture* target = videoTextures_[videoTextureIndex_];

    if (videoUploadsYuv_) {
        // SDL's YUV shader converts, so the CPU only copies: 1.5 bytes a
        // pixel against the 4 an RGB conversion writes.
        const uint64_t uploadStart = Platform::NowMs();
        const uint8_t* uPlane = frame.chroma.data();
        const uint8_t* vPlane = uPlane + frame.vPlaneOffset();
        const int result = SDL_UpdateYUVTexture(target, nullptr,
                                                frame.luma.data(), frame.lumaStride,
                                                uPlane, frame.chromaStride,
                                                vPlane, frame.chromaStride);
        if (result != 0) {
            LOGF("[player] YUV upload failed: %s", SDL_GetError());
            return false;
        }

        videoTexture_ = target;
        statUploadMs_ += (double)(Platform::NowMs() - uploadStart);
        ++statFrames_;
        logPlaybackStats();
        return true;
    }

    // Convert into ordinary memory, then hand the texture one sequential
    // copy. Mapped texture memory is write-combined, which makes the
    // conversion's scattered byte writes much more expensive than the extra
    // bulk copy costs.
#if defined(BJ_VIDEO_YUY2)
    const int stagingStride = frame.width * 2;
#else
    const int stagingStride = frame.width * 4;
#endif
    const size_t stagingBytes = (size_t)stagingStride * frame.height;
    if (videoStaging_.size() != stagingBytes) videoStaging_.resize(stagingBytes);

    const uint64_t convertStart = Platform::NowMs();
#if defined(BJ_VIDEO_YUY2)
    PackNv12ToYuy2(frame, videoStaging_.data(), stagingStride);
#else
    videoConverter_.convert(frame, videoStaging_.data(), stagingStride,
                            ColorSpaceForHeight(frame.height), videoBytes_);
#endif
    const uint64_t convertEnd = Platform::NowMs();

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(target, nullptr, &pixels, &pitch) != 0) return false;

    if (pitch == stagingStride) {
        std::memcpy(pixels, videoStaging_.data(), stagingBytes);
    } else {
        // Padded rows: copy one at a time rather than assuming a stride.
        uint8_t* out = static_cast<uint8_t*>(pixels);
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(out + (size_t)y * pitch,
                        videoStaging_.data() + (size_t)y * stagingStride,
                        (size_t)stagingStride);
        }
    }
    SDL_UnlockTexture(target);
    videoTexture_ = target;
    const uint64_t unlockEnd = Platform::NowMs();

    // Convert is the color conversion alone; upload is what lock and unlock
    // cost around it, which on this renderer includes tiling the result.
    statConvertMs_ += (double)(convertEnd - convertStart);
    statUploadMs_  += (double)(unlockEnd - convertEnd);
    ++statFrames_;

    logPlaybackStats();
    return true;
}

// Averages of what the last couple of seconds cost per frame. Both are what
// the main thread spent: convert is the color conversion, upload is what
// getting the result into a texture cost around it.
void App::logPlaybackStats()
{
    // The console reopens the log for every line.
    if (!settings_.diagnostics) return;
    if (!player_ || statFrames_ <= 0) return;

    const uint64_t now = Platform::NowMs();
    if (statLastLogMs_ == 0) statLastLogMs_ = now;
    if (now - statLastLogMs_ < 2000) return;

    // Frames is what reached the screen; loops is how fast the main thread
    // went round, and one overrunning 16.7ms costs a whole vblank.
    const int loops = statLoops_ > 0 ? statLoops_ : 1;
    const int decMs = player_ ? player_->takeDecodeMs() : 0;
    const int decFr = player_ ? player_->takeDecodeFrames() : 0;
    LOGF("[player] %d frames in %lums: decode %s ms, convert %s ms, upload %s ms, "
         "queue %d, segments %d, decoded %d, dropped %d",
         statFrames_, (unsigned long)(now - statLastLogMs_),
         bj::ToString(decFr > 0 ? (double)decMs / decFr : 0.0, 1).c_str(),
         bj::ToString(statConvertMs_ / statFrames_, 1).c_str(),
         bj::ToString(statUploadMs_ / statFrames_, 1).c_str(),
         player_->queuedFrames(), player_->bufferedSegments(),
         player_->decodedFrames(), player_->droppedFrames());
    LOGF("[player] %d loops in %lums: draw %s ms, present %s ms",
         statLoops_, (unsigned long)(now - statLastLogMs_),
         bj::ToString(statDrawMs_ / loops, 1).c_str(),
         bj::ToString(statPresentSumMs_ / loops, 1).c_str());
    statFrames_ = 0;
    statLoops_  = 0;
    statConvertMs_ = statUploadMs_ = 0.0;
    statDrawMs_ = statPresentSumMs_ = 0.0;
    statLastLogMs_ = now;
}

void App::signOut()
{
    pool_.submit([this] {
        client_.signOut();
        pool_.post([this] {
            libraries_.clear();
            items_.clear();
            art_->clear();
            toast("Signed out");
            beginDiscovery();
        });
    });
}

// ----------------------------------------------------------------- input

Action App::translate(const SDL_Event& event) const
{
    if (event.type == SDL_QUIT) return Action::Quit;

    if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
            case SDLK_UP:     case SDLK_w: return Action::Up;
            case SDLK_DOWN:   case SDLK_s: return Action::Down;
            case SDLK_LEFT:   case SDLK_a: return Action::Left;
            case SDLK_RIGHT:  case SDLK_d: return Action::Right;
            case SDLK_RETURN: case SDLK_SPACE: return Action::Accept;
            case SDLK_ESCAPE: case SDLK_BACKSPACE: return Action::Back;
            case SDLK_TAB:    return Action::Menu;
            case SDLK_r:      return Action::Refresh;
            case SDLK_x:      return Action::Options;
            case SDLK_PAGEUP:   return Action::PageUp;
            case SDLK_PAGEDOWN: return Action::PageDown;
            case SDLK_F1:     return Action::None;   // handled below as debug
            default: return Action::None;
        }
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (event.cbutton.button) {
            // Directions are handled by the repeat pump, not here.
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Action::None;
            // USE_BUTTON_LABELS is set, so BUTTON_A is the button labeled
            // A rather than the one in the Xbox A position.
            case SDL_CONTROLLER_BUTTON_A:          return Action::Accept;
            case SDL_CONTROLLER_BUTTON_B:          return Action::Back;
            case SDL_CONTROLLER_BUTTON_Y:          return Action::Refresh;
            case SDL_CONTROLLER_BUTTON_X:          return Action::Options;
            case SDL_CONTROLLER_BUTTON_START:      return Action::Menu;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return Action::PageUp;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Action::PageDown;
            default: return Action::None;
        }
    }

    return Action::None;
}

void App::openController(int deviceIndex)
{
    if (!SDL_IsGameController(deviceIndex)) {
        LOGF("[input] %d: \"%s\" has no mapping, ignoring",
             deviceIndex, SDL_JoystickNameForIndex(deviceIndex));
        return;
    }
    SDL_GameController* pad = SDL_GameControllerOpen(deviceIndex);
    if (!pad) return;
    controllers_.push_back(pad);
    LOGF("[input] %s connected", SDL_GameControllerName(pad));
}

void App::closeController(SDL_JoystickID which)
{
    for (auto it = controllers_.begin(); it != controllers_.end(); ++it) {
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(*it);
        if (joystick && SDL_JoystickInstanceID(joystick) == which) {
            LOGF("[input] %s disconnected", SDL_GameControllerName(*it));
            SDL_GameControllerClose(*it);
            controllers_.erase(it);
            return;
        }
    }
}

// Once immediately, then repeating after a pause.
void App::pumpHeldDirection()
{
    // The stick wins when both are pushed, since it is the one being held
    // deliberately.
    const Action current = (stickDirection_ != Action::None) ? stickDirection_
                                                             : dpadDirection_;

    if (current == Action::None) {
        heldDirection_ = Action::None;
        return;
    }

    const uint64_t now = Platform::NowMs();
    if (current != heldDirection_) {
        heldDirection_    = current;
        heldNextRepeatMs_ = now + 420;   // pause before repeating
        handleAction(current);
        return;
    }

    if (now >= heldNextRepeatMs_) {
        heldNextRepeatMs_ = now + 110;   // then briskly
        handleAction(current);
    }
}

void App::handleAction(Action action)
{
    if (action == Action::Quit) { running_ = false; return; }
    if (action == Action::None) return;

    switch (screen_) {
        case Screen::Connecting:
            if (action == Action::Accept && !errorLine_.empty()) startup();
            break;
        case Screen::Welcome:      handleWelcomeAction(action); break;
        case Screen::ServerSelect: handleServerSelectAction(action); break;
        case Screen::SignIn:
            if (action == Action::Back) { client_.quickConnectCancel(); beginDiscovery(); }
            else if (action == Action::Accept && quickConnectCode_.empty()) {
                beginSignIn(client_.serverUrl());
            }
            break;
        case Screen::Home:     handleHomeAction(action); break;
        case Screen::Settings: handleSettingsAction(action); break;
        case Screen::About:
            if (action == Action::Back || action == Action::Accept) {
                screen_ = Screen::Settings;
            }
            break;
        case Screen::Browse:
            if (keyboardOpen_) handleKeyboardAction(action);
            else               handleBrowseAction(action);
            break;
        case Screen::Detail: handleDetailAction(action); break;
        case Screen::Playing: handlePlayerAction(action); break;
    }
}

void App::handleServerSelectAction(Action action)
{
    if (discovering_) return;

    switch (action) {
        case Action::Up:
            if (!servers_.empty() && serverIndex_ > 0) --serverIndex_;
            break;
        case Action::Down:
            // One past the end is the manual entry row.
            if (serverIndex_ < (int)servers_.size()) ++serverIndex_;
            break;
        case Action::Refresh:
            beginDiscovery();
            break;
        case Action::Accept:
            if (serverIndex_ >= (int)servers_.size()) { beginManualServer(); break; }
            beginSignIn(servers_[serverIndex_].address);
            break;
        default: break;
    }
}

// Settings rows, in the order they are drawn.
namespace {
#if defined(__WIIU__)
  #define BJ_HAS_SECOND_SCREEN 1
#else
  #define BJ_HAS_SECOND_SCREEN 0
#endif


// 0 is direct: the server sends what it has, with no re-encode.
std::string QualityLabel(int height)
{
    if (height <= 0) return "Direct";
    return bj::ToString(height) + "p";
}

const char* DisplayLabel(Settings::Display display)
{
    switch (display) {
        case Settings::Display::TvOnly:      return "TV only";
        case Settings::Display::GamepadOnly: return "GamePad only";
        default:                             return "TV and GamePad";
    }
}
}  // namespace

void App::switchServer()
{
    // Released, not signed out: the token stays on disk under this server.
    pool_.submit([this] {
        client_.releaseSession();
        pool_.post([this] {
            libraries_.clear();
            sidebar_.clear();
            items_.clear();
            homeRows_.clear();
            art_->clear();
            beginDiscovery();
        });
    });
}

// Sidebar up and down. `canLeave` is false where there is nothing to the
// right to move into. True when the action was the sidebar's.
bool App::handleSidebarNav(Action action, bool canLeave)
{
    if (!sidebarFocused_) return false;

    switch (action) {
        case Action::Up:
            if (libraryIndex_ > 0) openSidebarEntry(libraryIndex_ - 1);
            return true;
        case Action::Down:
            if (libraryIndex_ + 1 < (int)sidebar_.size())
                openSidebarEntry(libraryIndex_ + 1);
            return true;
        case Action::Accept:
            // Search takes a query rather than handing focus to a grid.
            if (libraryIndex_ < (int)sidebar_.size() &&
                sidebar_[libraryIndex_].kind == SidebarEntry::Kind::Search) {
                beginSearch();
                return true;
            }
            if (canLeave) sidebarFocused_ = false;
            return true;
        case Action::Right:
            if (canLeave) sidebarFocused_ = false;
            return true;
        default:
            return true;   // the sidebar swallows the rest while focused
    }
}

bool App::handleServiceTabs(Action action)
{
    // Falling off the bottom of the sidebar lands on the bar.
    if (sidebarFocused_ && !bottomBarFocused_ && action == Action::Down &&
        libraryIndex_ + 1 >= (int)sidebar_.size()) {
        bottomBarFocused_ = true;
        bottomRow_ = 0;   // the server row is the first thing below the list
        // Start on the tab that is already in use.
        bottomTab_ = 0;
        return true;
    }

    if (!bottomBarFocused_) return false;

    const int lastTab = kServiceTabCount;   // the gear
    switch (action) {
        case Action::Left:
            if (bottomRow_ == 1 && bottomTab_ > 0) --bottomTab_;
            return true;
        case Action::Right:
            if (bottomRow_ == 1 && bottomTab_ < lastTab) ++bottomTab_;
            return true;
        case Action::Down:
            if (bottomRow_ == 0) bottomRow_ = 1;
            return true;
        case Action::Up:
            // Server row sits above the tabs; above that is the list again.
            if (bottomRow_ == 1) { bottomRow_ = 0; return true; }
            bottomBarFocused_ = false;
            sidebarFocused_   = true;
            return true;
        case Action::Back:
            bottomBarFocused_ = false;
            sidebarFocused_   = true;
            return true;
        case Action::Accept:
            if (bottomRow_ == 0) {
                bottomBarFocused_ = false;
                switchServer();
            } else if (bottomTab_ == lastTab) {
                bottomBarFocused_ = false;
                sidebarFocused_   = false;
                screen_      = Screen::Settings;
                settingsRow_ = 0;
            } else {
                switchSource(kServiceTabs[bottomTab_].source);
            }
            return true;
        default:
            return true;   // the bar swallows the rest while it has focus
    }
}

void App::handleSettingsAction(Action action)
{
    if (keyboardOpen_) { handleKeyboardAction(action); return; }
    if (!seerrCode_.empty() && action == Action::Back) {
        seerrCode_.clear();
        seerr_.quickConnectCancel();
        return;
    }
    if (handleServiceTabs(action)) return;
    if (handleSidebarNav(action, true)) return;

    const std::vector<SettingsEntry> rows = settingsRows();
    const int count = (int)rows.size();
    if (settingsRow_ >= count) settingsRow_ = count - 1;

    if (settingsRow_ < count &&
        rows[(size_t)settingsRow_].action == SettingAction::Heading) {
        settingsRow_ = nextSettingsRow(rows, settingsRow_, 1);
    }

    switch (action) {
        case Action::Up:
            settingsRow_ = nextSettingsRow(rows, settingsRow_, -1);
            return;
        case Action::Down:
            settingsRow_ = nextSettingsRow(rows, settingsRow_, 1);
            return;
        case Action::Left:
        case Action::Back:
            sidebarFocused_ = true;
            return;
        case Action::Accept:
            break;
        default:
            return;
    }

    switch (rows[(size_t)settingsRow_].action) {
        case SettingAction::JellyfinServer:
            switchServer();
            break;

        case SettingAction::JellyfinAccount:
            // Asked twice: signing out means Quick Connect again.
            if (confirmSignOut_ && Platform::NowMs() < confirmUntilMs_) {
                confirmSignOut_ = false;
                signOut();
            } else {
                confirmSignOut_ = true;
                confirmUntilMs_ = Platform::NowMs() + 5000;
                toast("Press A again to sign out");
            }
            break;

        case SettingAction::RequestAlongside:
            if (!settings_.requestServerElsewhere) {
                // Already the choice, so this means look again.
                seerrAutoTried_ = false;
                tryRequestServerBesideJellyfin();
                toast("Looking for Seerr beside Jellyfin");
                break;
            }
            settings_.requestServerElsewhere = false;
            settings_.save();
            seerrAutoTried_ = false;
            tryRequestServerBesideJellyfin();
            break;

        case SettingAction::RequestElsewhere:
            if (settings_.requestServerElsewhere) break;
            settings_.requestServerElsewhere = true;
            settings_.save();
            // What was found beside the library is not the one they mean.
            seerr_.signOut();
            seerr_.setServer("");
            seerrAutoTried_ = true;
            rebuildSidebar();
            break;

        case SettingAction::RequestAddress:
            beginTextEntry(KeyboardFor::RequestServerAddress);
            searchTerm_ = seerrFormAddress_;
            break;
        case SettingAction::RequestUser:
            beginTextEntry(KeyboardFor::RequestUser);
            searchTerm_ = seerrFormUser_;
            break;
        case SettingAction::RequestPassword:
            beginTextEntry(KeyboardFor::RequestPassword);
            searchTerm_ = seerrFormPassword_;
            break;

        case SettingAction::RequestTest:  testRequestServer(false); break;
        case SettingAction::RequestSave:  testRequestServer(true);  break;

#if BJ_HAS_SECOND_SCREEN
        case SettingAction::Display:
            // Fixed when the window is created, so this waits for a relaunch.
            settings_.display =
                (settings_.display == Settings::Display::TvAndGamepad)
                    ? Settings::Display::TvOnly
                : (settings_.display == Settings::Display::TvOnly)
                    ? Settings::Display::GamepadOnly
                    : Settings::Display::TvAndGamepad;
            settings_.save();
            toast(std::string(DisplayLabel(settings_.display)) +
                  " - takes effect next launch");
            break;
#endif

        case SettingAction::Playback: {
            int at = 0;
            while (at < kQualityStepCount && kQualitySteps[at] != settings_.playbackHeight) {
                ++at;
            }
            do {
                at = (at + 1) % kQualityStepCount;
            } while (kPlaybackMaxHeight > 0 && kQualitySteps[at] > kPlaybackMaxHeight);
            settings_.playbackHeight = kQualitySteps[at];
            settings_.save();
            applyStreamPrefs();
            break;
        }

        case SettingAction::VideoBitrate: {
            int at = 0;
            while (at < kVideoBitrateStepCount &&
                   kVideoBitrateSteps[at] != settings_.videoBitrate) {
                ++at;
            }
            at = (at + 1) % kVideoBitrateStepCount;
            settings_.videoBitrate = kVideoBitrateSteps[at];
            settings_.save();
            applyStreamPrefs();
            break;
        }

        case SettingAction::Framerate: {
            int at = 0;
            while (at < kFramerateStepCount &&
                   kFramerateSteps[at] != settings_.maxFramerate) {
                ++at;
            }
            do {
                at = (at + 1) % kFramerateStepCount;
            } while (BJ_MAX_FRAMERATE > 0 &&
                     kFramerateSteps[at] > BJ_MAX_FRAMERATE);
            settings_.maxFramerate = kFramerateSteps[at];
            settings_.save();
            applyStreamPrefs();
            break;
        }

        case SettingAction::AudioFormat:
            // A build that decodes one format has nothing to choose between.
            if (!BJ_AUDIO_AAC) { toast("This build decodes MP3 only"); break; }
            settings_.audioFormat =
                (settings_.audioFormat == Settings::AudioFormat::Automatic)
                    ? Settings::AudioFormat::Aac
              : (settings_.audioFormat == Settings::AudioFormat::Aac)
                    ? Settings::AudioFormat::Mp3
                    : Settings::AudioFormat::Automatic;
            settings_.save();
            applyStreamPrefs();
            break;

        case SettingAction::AudioBitrate: {
            int at = 0;
            while (at < kAudioBitrateStepCount &&
                   kAudioBitrateSteps[at] != settings_.audioBitrate) {
                ++at;
            }
            at = (at + 1) % kAudioBitrateStepCount;
            settings_.audioBitrate = kAudioBitrateSteps[at];
            settings_.save();
            applyStreamPrefs();
            break;
        }

        case SettingAction::Diagnostics:
            settings_.diagnostics = !settings_.diagnostics;
            settings_.save();
            if (!settings_.diagnostics) showDebug_ = false;
            break;

        case SettingAction::DiagnosticsNote:
            break;

        case SettingAction::About:
            screen_ = Screen::About;
            break;

        case SettingAction::Heading:
            break;
#if !BJ_HAS_SECOND_SCREEN
        case SettingAction::Display:
            break;
#endif
    }

}

void App::handleHomeAction(Action action)
{
    if (handleServiceTabs(action)) return;
    if (action == Action::Refresh) {
        if (showingSeerr_) loadDiscover();
        else               loadHome();
        return;
    }

    if (handleSidebarNav(action, !homeRows_.empty())) return;

    if (homeRows_.empty()) { sidebarFocused_ = true; return; }
    HomeRow& row = homeRows_[homeRow_];

    switch (action) {
        case Action::Left:
            if (row.focus > 0) --row.focus;
            else sidebarFocused_ = true;
            break;
        case Action::Right:
            if (row.focus + 1 < (int)row.items.size()) ++row.focus;
            break;
        case Action::Up:
            if (homeRow_ > 0) --homeRow_;
            break;
        case Action::Down:
            if (homeRow_ + 1 < (int)homeRows_.size()) ++homeRow_;
            break;
        case Action::PageUp:
            row.focus = (row.focus > homeColumns()) ? row.focus - homeColumns() : 0;
            break;
        case Action::PageDown:
            row.focus = (row.focus + homeColumns() < (int)row.items.size())
                      ? row.focus + homeColumns()
                      : (int)row.items.size() - 1;
            break;
        case Action::Back:
            sidebarFocused_ = true;
            break;
        case Action::Accept:
            if (row.focus < (int)row.items.size()) {
                const JfItem& item = row.items[row.focus];
                if (item.isFolder && item.type == "Series") {
                    detailItem_ = item;
                    screen_ = Screen::Detail;
                } else {
                    detailItem_ = item;
                    screen_ = Screen::Detail;
                }
            }
            break;
        default: break;
    }

    // Keep the focused tile inside the four that are drawn.
    for (HomeRow& r : homeRows_) {
        if (r.items.empty()) { r.scroll = 0; continue; }
        if (r.focus < r.scroll) r.scroll = r.focus;
        if (r.focus > r.scroll + homeColumns() - 1) r.scroll = r.focus - homeColumns() + 1;
        const int maxScroll = (int)r.items.size() - homeColumns();
        if (r.scroll > maxScroll) r.scroll = maxScroll;
        if (r.scroll < 0) r.scroll = 0;
    }

    // Keep the focused tile on screen.
    const int visible = 5;
    if (row.focus < row.scroll) row.scroll = row.focus;
    if (row.focus >= row.scroll + visible) row.scroll = row.focus - visible + 1;
}

bool App::onSearchScreen() const
{
    return libraryIndex_ >= 0 && libraryIndex_ < (int)sidebar_.size() &&
           sidebar_[libraryIndex_].kind == SidebarEntry::Kind::Search;
}

namespace {

// The last row holds the wide keys, handled by name.
const char* const kKeyRows[] = {
    "1234567890.",
    "abcdefghijk",
    "lmnopqrstuv",
    "wxyz-_:/'&@",
};
constexpr int kKeyRowCount = 4;
constexpr int kKeyColCount = 11;

// Row 4: space, backspace, done.
constexpr int kActionRow = kKeyRowCount;

}  // namespace

// For a server discovery cannot see. Closing the keyboard commits.
void App::commitManualServer()
{
    std::string address = searchTerm_;
    searchTerm_.clear();
    keyboardFor_ = KeyboardFor::Search;

    while (!address.empty() && address.back() == ' ') address.pop_back();
    if (address.empty()) { screen_ = Screen::ServerSelect; return; }
    if (address.find("://") == std::string::npos) address = "http://" + address;

    beginSignIn(address);
}

std::string App::requestServerSummary() const
{
    if (!seerr_.configured()) return "not set up";

    std::string where = seerr_.baseUrl();
    if (where.rfind("http://", 0) == 0) where = where.substr(7);
    if (!seerr_.userName().empty()) return seerr_.userName() + " at " + where;
    return where;
}

bool App::onScreenKeyboardWanted() const
{
#if defined(__WIIU__) || defined(_XBOX)
    return true;
#else
    // Follows whatever was last touched, so picking up a pad mid-session
    // brings the drawn keyboard with it.
    return inputScheme_ != InputScheme::Keyboard;
#endif
}

void App::beginTextEntry(KeyboardFor purpose)
{
    keyboardFor_    = purpose;
    searchTerm_.clear();
    sidebarFocused_ = false;
    keyRow_ = 1;
    keyCol_ = 0;

    if (onScreenKeyboardWanted()) {
        keyboardOpen_ = true;
        typingDirect_ = false;
        return;
    }
    keyboardOpen_ = false;
    typingDirect_ = true;
    SDL_StartTextInput();
}

void App::commitTextEntry()
{
    if (typingDirect_) { typingDirect_ = false; SDL_StopTextInput(); }
    keyboardOpen_ = false;

    switch (keyboardFor_) {
        case KeyboardFor::RequestServerAddress:
            // From Settings this only fills the form in; from anywhere else
            // it is the whole of the setup.
            if (settings_.requestServerElsewhere) {
                seerrFormAddress_ = searchTerm_;
                searchTerm_.clear();
            } else {
                commitRequestServer();
            }
            break;
        case KeyboardFor::RequestUser:
            seerrFormUser_ = searchTerm_;
            searchTerm_.clear();
            break;
        case KeyboardFor::RequestPassword:
            seerrFormPassword_ = searchTerm_;
            searchTerm_.clear();
            break;
        case KeyboardFor::ServerAddress: commitManualServer(); break;
        case KeyboardFor::Search:
            if (!searchTerm_.empty()) runSearch(searchTerm_);
            break;
    }
}

void App::cancelTextEntry()
{
    if (typingDirect_) { typingDirect_ = false; SDL_StopTextInput(); }
    keyboardOpen_ = false;
    if (keyboardFor_ != KeyboardFor::Search) keyboardFor_ = KeyboardFor::Search;
}

void App::applyStreamPrefs()
{
    JellyfinClient::StreamPrefs prefs;
    prefs.videoBitrate = settings_.videoBitrate;
    prefs.maxFramerate = settings_.maxFramerate;
    prefs.audioBitrate = settings_.audioBitrate;
    switch (settings_.audioFormat) {
        case Settings::AudioFormat::Aac: prefs.audioCodecs = "aac"; break;
        case Settings::AudioFormat::Mp3: prefs.audioCodecs = "mp3"; break;
        case Settings::AudioFormat::Automatic: break;
    }
    client_.setStreamPrefs(prefs);
}

void App::testRequestServer(bool keep)
{
    if (seerrFormAddress_.empty()) { toast("Set an address first"); return; }

    const std::string address = seerrFormAddress_;
    const std::string user    = seerrFormUser_;
    const std::string pass    = seerrFormPassword_;

    toast(keep ? "Signing in" : "Testing");
    pool_.submit([this, address, user, pass, keep] {
        // A copy, so a test that fails leaves the live one alone.
        SeerrClient probe;
        probe.setServer(address);

        std::string error;
        bool ok = false;
        if (user.empty() && pass.empty()) {
            ok = probe.reachable(error);
            if (ok && !keep) error = "reached, but no account given";
        } else {
            ok = probe.signInWithPassword(user, pass, error, false);
        }

        if (!ok) {
            pool_.post([this, error] { toast(error); });
            return;
        }
        if (!keep) {
            pool_.post([this, error] {
                toast(error.empty() ? "That works" : error);
            });
            return;
        }

        // Only now does the live client take it on, so Save is the only
        // thing that changes what the app is signed in to.
        seerr_.setServer(address);
        std::string signInError;
        if (!seerr_.signInWithPassword(user, pass, signInError)) {
            pool_.post([this, signInError] { toast(signInError); });
            return;
        }
        pool_.post([this] {
            toast("Saved");
            rebuildSidebar();
        });
    });
}

void App::tryRequestServerBesideJellyfin()
{
    // Guessing would reach the wrong machine and sign in to it.
    if (settings_.requestServerElsewhere) return;
    if (seerrAutoTried_ || seerr_.configured() || !client_.signedIn()) return;
    seerrAutoTried_ = true;

    const std::string guess = SeerrClient::GuessAddressFor(client_);
    if (guess.empty()) return;

    pool_.submit([this, guess] {
        // Only where the library is, not a search of the network.
        seerr_.setServer(guess);
        std::string error;
        if (!seerr_.signInWithJellyfin(client_, error)) {
            LOGF("[seerr] nothing at %s: %s", guess.c_str(), error.c_str());
            seerr_.setServer("");
            return;
        }
        pool_.post([this] {
            LOGF("[seerr] signed in beside the library");
            rebuildSidebar();
        });
    });
}

void App::beginRequestServer()
{
    beginTextEntry(KeyboardFor::RequestServerAddress);
}

void App::commitRequestServer()
{
    std::string address = searchTerm_;
    searchTerm_.clear();
    keyboardFor_ = KeyboardFor::Search;
    keyboardOpen_ = false;

    while (!address.empty() && address.back() == ' ') address.pop_back();
    if (address.empty()) return;

    seerr_.setServer(address);
    seerrCode_.clear();
    toast("Reaching the request server");

    pool_.submit([this] {
        std::string error;
        if (!seerr_.reachable(error)) {
            pool_.post([this, error] { toast(error); });
            return;
        }
        std::string code;
        if (!seerr_.quickConnectStart(code, error)) {
            pool_.post([this, error] { toast(error); });
            return;
        }
        pool_.post([this, code] {
            seerrCode_       = code;
            seerrPollAtMs_   = Platform::NowMs() + 2000;
            seerrDeadlineMs_ = Platform::NowMs() + 5 * 60 * 1000;
        });
    });
}

void App::pollRequestQuickConnect()
{
    if (seerrCode_.empty() || seerrPollInFlight_) return;

    const uint64_t now = Platform::NowMs();
    if (now > seerrDeadlineMs_) {
        seerrCode_.clear();
        seerr_.quickConnectCancel();
        toast("The code expired");
        return;
    }
    if (now < seerrPollAtMs_) return;

    seerrPollAtMs_ = now + 2000;
    seerrPollInFlight_ = true;
    pool_.submit([this] {
        std::string error;
        const bool approved = seerr_.quickConnectApproved(error);
        std::string finishError;
        const bool done = approved && seerr_.quickConnectFinish(finishError);
        pool_.post([this, approved, done, error, finishError] {
            seerrPollInFlight_ = false;
            if (!approved) {
                // An expired code is the only failure worth interrupting for.
                if (!error.empty()) { seerrCode_.clear(); toast(error); }
                return;
            }
            seerrCode_.clear();
            if (!done) { toast(finishError); return; }
            toast("Seerr ready");
            rebuildSidebar();
        });
    });
}

void App::beginManualServer()
{
    beginTextEntry(KeyboardFor::ServerAddress);
}

void App::beginSearch()
{
    curKind_  = LevelKind::Library;
    curTitle_ = "Search";
    navStack_.clear();
    beginTextEntry(KeyboardFor::Search);
}

void App::handleKeyboardAction(Action action)
{
    switch (action) {
        case Action::Up:
            if (keyRow_ > 0) {
                const bool leavingActions = (keyRow_ == kActionRow);
                --keyRow_;
                // The wide keys sit under roughly these columns, so coming
                // back up lands near where the eye already is.
                if (leavingActions) {
                    static const int kColumnUnderAction[3] = { 1, 5, 8 };
                    keyCol_ = kColumnUnderAction[keyCol_ < 3 ? keyCol_ : 2];
                }
            }
            break;
        case Action::Down:
            if (keyRow_ < kActionRow) {
                ++keyRow_;
                // Three wide keys down there, not ten narrow ones.
                if (keyRow_ == kActionRow && keyCol_ > 2) keyCol_ = 2;
            }
            break;
        case Action::Left:
            if (keyCol_ > 0) --keyCol_;
            break;
        case Action::Right: {
            const int limit = (keyRow_ == kActionRow) ? 2 : kKeyColCount - 1;
            if (keyCol_ < limit) ++keyCol_;
            break;
        }
        case Action::Accept:
            if (keyRow_ == kActionRow) {
                switch (keyCol_) {
                    case 0: searchTerm_ += ' '; break;
                    case 1:
                        if (!searchTerm_.empty()) {
                            do {
                                searchTerm_.pop_back();
                            } while (!searchTerm_.empty() &&
                                     ((unsigned char)searchTerm_.back() & 0xC0) == 0x80);
                        }
                        break;
                    case 2:
                        if (keyboardFor_ != KeyboardFor::Search) {
                            commitTextEntry();
                            return;
                        }
                        keyboardOpen_ = false;
                        break;
                }
            } else if (keyCol_ < kKeyColCount) {
                searchTerm_ += kKeyRows[keyRow_][keyCol_];
            }
            if (keyboardFor_ != KeyboardFor::Search) break;
            // Results follow along as the term is typed, so there is no
            // separate moment of submitting.
            if (!searchTerm_.empty()) runSearch(searchTerm_);
            else items_.clear();
            break;
        case Action::Refresh:   // Y is a quicker backspace
            if (!searchTerm_.empty()) {
                do {
                    searchTerm_.pop_back();
                } while (!searchTerm_.empty() &&
                         ((unsigned char)searchTerm_.back() & 0xC0) == 0x80);
                if (keyboardFor_ != KeyboardFor::Search) break;
                if (searchTerm_.empty()) items_.clear();
                else runSearch(searchTerm_);
            }
            break;
        case Action::Back:
        case Action::Menu:
            keyboardOpen_ = false;
            break;
        default:
            break;
    }
}

void App::drawKeyboard()
{
    // Keys shrink to whatever the content area is, so the grid fits a 640
    // line screen as well as a wide one.
    const int gap = 8;
    const int area = logicalW_ - kSidebarWidth - 40;
    int keyW = (area - (kKeyColCount - 1) * gap) / kKeyColCount;
    if (keyW > 62) keyW = 62;
    const int keyH = (keyW * 54) / 62;

    const int gridW = kKeyColCount * keyW + (kKeyColCount - 1) * gap;
    const int originX = kSidebarWidth + (logicalW_ - kSidebarWidth - gridW) / 2;
    const int originY = kTopBarHeight + 108;

    const SDL_Rect panel = { originX - 20, originY - 20, gridW + 40,
                             (kKeyRowCount + 1) * (keyH + gap) + 32 };
    render_.fillRoundedRect(panel, 12, Palette::Panel);
    render_.strokeRect(panel, Palette::PanelHi, 2);

    for (int row = 0; row < kKeyRowCount; ++row) {
        for (int col = 0; col < kKeyColCount; ++col) {
            const SDL_Rect key = { originX + col * (keyW + gap),
                                   originY + row * (keyH + gap), keyW, keyH };
            const bool focused = (row == keyRow_ && col == keyCol_);
            render_.fillRoundedRect(key, 6, focused ? Palette::Accent : Palette::PanelHi);
            const char letter[2] = { kKeyRows[row][col], '\0' };
            render_.drawText(letter, key.x + keyW / 2, key.y + 12,
                             FontSize::Body, Palette::Text, Align::Center);
        }
    }

    // Space, backspace and done, sized to what they say.
    const int actionY = originY + kKeyRowCount * (keyH + gap);
    // Proportions of the grid, so these shrink with the keys above them.
    const struct { const char* label; int width; } actions[] = {
        { "Space",  (gridW * 2) / 5 }, { "Delete", (gridW * 3) / 10 },
        { "Done",   (gridW * 3) / 10 },
    };
    int x = originX;
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect key = { x, actionY, actions[i].width, keyH };
        const bool focused = (keyRow_ == kActionRow && keyCol_ == i);
        render_.fillRoundedRect(key, 6, focused ? Palette::Accent : Palette::PanelHi);
        render_.drawText(actions[i].label, key.x + actions[i].width / 2, key.y + 12,
                         FontSize::Body, Palette::Text, Align::Center);
        x += actions[i].width + gap;
    }
}

void App::runSearch(const std::string& term)
{
    if (term.empty()) return;
    searchRunning_ = true;
    itemsLoading_  = true;
    const int requestId = ++itemsRequestId_;

    pool_.submit([this, term, requestId] {
        std::string error;
        std::vector<JfItem> found = showingSeerr_ ? seerr_.search(term, error)
                                                  : client_.search(term, error);
        LOGF("[search] \"%s\": %lu result(s)", term.c_str(), (unsigned long)found.size());
        pool_.post([this, found, requestId] {
            if (requestId != itemsRequestId_) return;
            items_ = found;
            itemIndex_ = 0;
            gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
            itemsLoading_ = false;
            searchRunning_ = false;
        });
    });
}

void App::handleBrowseAction(Action action)
{
    if (browseMenuOpen_) { handleBrowseMenuAction(action); return; }

    // For a library, not a season listing.
    const bool discovering = (activeSource_ == MediaSource::Seerr);
    const bool genreApplies = discovering && !discoverGenres_.empty();
    if (action == Action::Options && curKind_ == LevelKind::Library &&
        (genreApplies || (!discovering && !curParentId_.empty()))) {
        browseMenuOpen_ = true;
        browseMenuTab_  = 0;
        browseMenuRow_  = 0;
        return;
    }

    if (handleServiceTabs(action)) return;
    if (action == Action::Refresh) {
        switch (curKind_) {
            case LevelKind::Library:  openSidebarEntry(libraryIndex_); break;
            case LevelKind::Seasons:  loadSeasons(curSeriesId_, curTitle_); break;
            case LevelKind::Episodes: loadEpisodes(curSeriesId_, curParentId_,
                                                   curTitle_); break;
            case LevelKind::Folder:   loadFolder(curParentId_, curTitle_); break;
        }
        return;
    }

    if (handleSidebarNav(action, !items_.empty())) return;

    const GridMetrics grid = gridMetrics();
    const int count = (int)items_.size();
    if (count == 0) { sidebarFocused_ = true; return; }

    switch (action) {
        case Action::Left:
            // Stepping off the left edge is how you get back to the sidebar.
            if (itemIndex_ % grid.columns == 0) sidebarFocused_ = true;
            else --itemIndex_;
            break;
        case Action::Right:
            if (itemIndex_ + 1 < count && (itemIndex_ + 1) % grid.columns != 0)
                ++itemIndex_;
            else if ((itemIndex_ + 1) % grid.columns == 0 && itemIndex_ + 1 < count)
                ++itemIndex_;   // wrap to the start of the next row
            break;
        case Action::Up:
            if (itemIndex_ >= grid.columns) itemIndex_ -= grid.columns;
            break;
        case Action::Down:
            if (itemIndex_ + grid.columns < count) itemIndex_ += grid.columns;
            else if (itemIndex_ / grid.columns < (count - 1) / grid.columns)
                itemIndex_ = count - 1;   // ragged last row
            break;
        case Action::PageUp:
            itemIndex_ = std::max(0, itemIndex_ - grid.columns * grid.visibleRows);
            break;
        case Action::PageDown:
            itemIndex_ = std::min(count - 1,
                                  itemIndex_ + grid.columns * grid.visibleRows);
            break;
        case Action::Accept:
            enterItem(itemIndex_);
            break;
        case Action::Back:
            // Back climbs out of a series before it returns to the sidebar.
            if (!popLevel()) sidebarFocused_ = true;
            break;
        default: break;
    }
    clampGridScroll();
    maybeLoadMore();
}

void App::handleDetailAction(Action action)
{
    // A Seerr item is not in the library, so it can only be asked for.
    if (detailItem_.source == MediaSource::Seerr) {
        if (action == Action::Back) { screen_ = Screen::Browse; return; }
        if (action != Action::Accept) return;

        const JfItem item = detailItem_;
        toast("Requesting " + item.name);
        pool_.submit([this, item] {
            std::string error;
            const bool ok = seerr_.request(item, error);
            pool_.post([this, item, ok, error] {
                toast(ok ? ("Requested " + item.name)
                         : (error.empty() ? "The request was refused" : error));
                if (!ok) return;
                for (JfItem& row : items_) {
                    if (row.id == item.id) row.requestStatus = 2;
                }
                for (HomeRow& hr : homeRows_) {
                    for (JfItem& row : hr.items) {
                        if (row.id == item.id) row.requestStatus = 2;
                    }
                }
                detailItem_.requestStatus = 2;
            });
        });
        return;
    }

    switch (action) {
        case Action::Back:
            screen_ = Screen::Browse;
            break;
        case Action::Accept:
            startPlayback(detailItem_, false);
            break;
        case Action::Refresh:
            // Y starts again from the beginning when a resume point exists.
            startPlayback(detailItem_, true);
            break;
        case Action::Options: {
            const bool nowFavorite = !detailItem_.isFavorite;
            const std::string itemId = detailItem_.id;
            detailItem_.isFavorite = nowFavorite;
            for (JfItem& item : items_) {
                if (item.id == itemId) item.isFavorite = nowFavorite;
            }
            toast(nowFavorite ? "Added to favorites" : "Removed from favorites");
            pool_.submit([this, itemId, nowFavorite] {
                client_.setFavorite(itemId, nowFavorite);
            });
            break;
        }
        case Action::Menu: {
            // Start marks watched or unwatched, and updates the tile without
            // waiting for the server to be asked again.
            const bool nowPlayed = !detailItem_.played;
            const std::string itemId = detailItem_.id;
            detailItem_.played = nowPlayed;
            detailItem_.resumeTicks = nowPlayed ? 0 : detailItem_.resumeTicks;
            detailItem_.playedFraction = nowPlayed ? 1.0 : 0.0;
            for (JfItem& item : items_) {
                if (item.id == itemId) {
                    item.played = nowPlayed;
                    item.resumeTicks = detailItem_.resumeTicks;
                    item.playedFraction = detailItem_.playedFraction;
                }
            }
            toast(nowPlayed ? "Marked watched" : "Marked unwatched");
            pool_.submit([this, itemId, nowPlayed] {
                client_.markPlayed(itemId, nowPlayed);
            });
            break;
        }
        default: break;
    }
}

// ------------------------------------------------------------- geometry

App::GridMetrics App::gridMetrics() const
{
    GridMetrics g{};
    const int padding   = 40;
    const int available = logicalW_ - kSidebarWidth - padding * 2;

    g.originX = kSidebarWidth + padding;
    g.originY = kTopBarHeight + 18;
    g.gapX    = 24;
    // Two lines of label live in this gap: title, then year. At 44 the year
    // was being overlapped by the next row of posters.
    g.gapY    = 60;

    // A breadcrumb sits above the grid once the user is inside a series.
    if (!navStack_.empty()) g.originY += 40;

    // The search field occupies the top of the content area, and results
    // were being drawn straight over it.
    if (onSearchScreen()) g.originY += 76;

    // A fixed four across. Posters stay large enough to read across a room,
    // and the library scrolls instead of the artwork shrinking to fit.
    // Episode stills are 16:9, so they get three wider tiles instead.
    const bool episodes = (curKind_ == LevelKind::Episodes);
    g.columns    = episodes ? 3 : kGridColumns;
    g.tileWidth  = (available - g.gapX * (g.columns - 1)) / g.columns;
    g.tileHeight = episodes ? (g.tileWidth * 9) / 16
                            : (g.tileWidth * 3) / 2;

    const int bodyHeight = logicalH_ - g.originY - kHintBarHeight - 8;
    const int rowStride  = g.tileHeight + g.gapY;
    g.visibleRows = std::max(1, bodyHeight / rowStride);
    return g;
}

void App::clampGridScroll()
{
    const GridMetrics g = gridMetrics();
    if (items_.empty()) { gridScrollTargetPx_ = 0.0f; return; }

    const int rowStride  = g.tileHeight + g.gapY;
    const int focusRow   = itemIndex_ / g.columns;
    const int totalRows  = ((int)items_.size() + g.columns - 1) / g.columns;
    const int bodyHeight = logicalH_ - g.originY - kHintBarHeight - 8;

    // Keep the focused row fully on screen, scrolling only as far as needed.
    const int rowTop    = focusRow * rowStride;
    const int rowBottom = rowTop + g.tileHeight + g.gapY;

    float target = gridScrollTargetPx_;
    if ((float)rowTop < target)                 target = (float)rowTop;
    if ((float)rowBottom > target + bodyHeight) target = (float)(rowBottom - bodyHeight);

    // Never scroll past the end of the list, and never above the start.
    const float maxScroll = std::max(0.0f, (float)(totalRows * rowStride - bodyHeight));
    if (target > maxScroll) target = maxScroll;
    if (target < 0.0f)      target = 0.0f;

    gridScrollTargetPx_ = target;
}

void App::updateScroll()
{
    const float delta = gridScrollTargetPx_ - gridScrollPx_;
    if (delta > -0.5f && delta < 0.5f) {
        gridScrollPx_ = gridScrollTargetPx_;
        return;
    }
    // Exponential ease. Frame-rate independence is not worth the complexity
    // here: both targets run at a locked 60Hz through vsync.
    gridScrollPx_ += delta * 0.28f;
}

// ------------------------------------------------------------------ frame

bool App::frame()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_WINDOWEVENT &&
            (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
             event.window.event == SDL_WINDOWEVENT_RESIZED)) {
            const int wasW = logicalW_, wasH = logicalH_;
            updateLayoutSize();
            if (logicalW_ != wasW || logicalH_ != wasH) {
                // Text was rasterized, and posters fetched, for the old size.
                render_.clearTextCache();
                LOGF("[ui] drawing at %dx%d", logicalW_, logicalH_);
            }
            rememberWindowSize();
        }

        // Typed text goes into the term directly, with no keyboard drawn.
        if (typingDirect_) {
            if (event.type == SDL_TEXTINPUT) {
                searchTerm_ += event.text.text;
                if (keyboardFor_ == KeyboardFor::Search && !searchTerm_.empty()) {
                    runSearch(searchTerm_);
                }
                continue;
            }
            if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_BACKSPACE) {
                    if (!searchTerm_.empty()) {
                        // One character, not one byte: the term is UTF-8.
                        do {
                            searchTerm_.pop_back();
                        } while (!searchTerm_.empty() &&
                                 ((unsigned char)searchTerm_.back() & 0xC0) == 0x80);
                        if (keyboardFor_ == KeyboardFor::Search) {
                            if (searchTerm_.empty()) items_.clear();
                            else runSearch(searchTerm_);
                        }
                    }
                    continue;
                }
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                    commitTextEntry();
                    continue;
                }
                if (key == SDLK_ESCAPE) { cancelTextEntry(); continue; }
            }
        }

        // Fullscreen, on the key every other player has it on.
        if (event.type == SDL_KEYDOWN &&
            (event.key.keysym.sym == SDLK_F11 ||
             (event.key.keysym.sym == SDLK_f &&
              (event.key.keysym.mod & (KMOD_GUI | KMOD_CTRL))))) {
            const Uint32 flags = SDL_GetWindowFlags(window_);
            const bool wasFull = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
            SDL_SetWindowFullscreen(window_,
                                    wasFull ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            settings_.fullscreen = !wasFull;
            settings_.save();
            continue;
        }

        // Hints follow whatever was last touched, not whatever is plugged in:
        // a pad left connected should not relabel a keyboard.
        if (!kFixedInputScheme) {
            if (event.type == SDL_KEYDOWN) {
                useInputScheme(InputScheme::Keyboard);
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                useInputScheme(SchemeForController(
                    SDL_GameControllerFromInstanceID(event.cbutton.which)));
            }
        }

        // Cafe OS hangs rather than backgrounds if an app keeps issuing GPU
        // work after the display is taken away. SDL sends both WILL* and DID*
        // per transition, so act only on an actual change of state.
        if (event.type == SDL_APP_WILLENTERBACKGROUND ||
            event.type == SDL_APP_DIDENTERBACKGROUND) {
            if (foreground_) {
                LOGF("[proc] entering background");
                foreground_ = false;
            }
            continue;
        }
        if (event.type == SDL_APP_WILLENTERFOREGROUND ||
            event.type == SDL_APP_DIDENTERFOREGROUND) {
            if (!foreground_) {
                LOGF("[proc] back in foreground");
                foreground_ = true;
                // Everything the GPU was holding is gone after a handover, so
                // drop the caches and let them rebuild from the disk copies.
                art_->clear();
                render_.clearTextCache();
            }
            continue;
        }
        if (event.type == SDL_APP_TERMINATING) {
            LOGF("[proc] terminating");
            running_ = false;
            return false;
        }

        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F1) {
            if (!settings_.diagnostics) {
                toast("Turn on diagnostics in Settings first");
            } else {
                showDebug_ = !showDebug_;
            }
            continue;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F2) {
            saveScreenshot();
            continue;
        }
        // A real keyboard still types, where there is one, but the on-screen
        // keyboard is what the console uses.
        if (event.type == SDL_TEXTINPUT && keyboardOpen_) {
            searchTerm_ += event.text.text;
            runSearch(searchTerm_);
            continue;
        }

        // Controllers arriving and leaving. A Pro Controller switched on
        // after the app started has to work, and so does the GamePad going
        // flat mid-film.
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            openController(event.cdevice.which);
            continue;
        }
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            closeController(event.cdevice.which);
            continue;
        }

        // D-pad direction, tracked rather than acted on, so holding it
        // repeats.
        if (event.type == SDL_CONTROLLERBUTTONDOWN ||
            event.type == SDL_CONTROLLERBUTTONUP) {
            const bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
            Action direction = Action::None;
            switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    direction = Action::Up; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  direction = Action::Down; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  direction = Action::Left; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: direction = Action::Right; break;
                default: break;
            }
            if (direction != Action::None) {
                dpadDirection_ = down ? direction : Action::None;
                continue;
            }
        }

        // Either stick steers. A dead zone well above the noise floor keeps
        // a worn stick from drifting through a library on its own.
        if (event.type == SDL_CONTROLLERAXISMOTION) {
            constexpr int kDeadZone = 16000;
            static int axisX = 0, axisY = 0;
            switch (event.caxis.axis) {
                case SDL_CONTROLLER_AXIS_LEFTX:
                case SDL_CONTROLLER_AXIS_RIGHTX: axisX = event.caxis.value; break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                case SDL_CONTROLLER_AXIS_RIGHTY: axisY = event.caxis.value; break;
                default: break;
            }
            if (std::abs(axisX) < kDeadZone && std::abs(axisY) < kDeadZone) {
                stickDirection_ = Action::None;
            } else if (std::abs(axisX) > std::abs(axisY)) {
                stickDirection_ = axisX > 0 ? Action::Right : Action::Left;
            } else {
                stickDirection_ = axisY > 0 ? Action::Down : Action::Up;
            }
            continue;
        }

        // The GamePad's "-" button, which nothing else uses.
        if (event.type == SDL_CONTROLLERBUTTONDOWN &&
            event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
            saveScreenshot();
            continue;
        }
        handleAction(translate(event));
        if (!running_) return false;
    }

    pumpHeldDirection();
    pool_.drainResults();

    // While backgrounded, keep pumping events so the system's messages are
    // still answered, but touch neither the GPU nor the screen.
    if (!foreground_) {
        SDL_Delay(16);
        return running_;
    }

    // Decoding and uploading artwork means creating SDL textures, which on
    // this renderer synchronizes with the GPU. None of it is needed while a
    // film is on screen.
    if (screen_ != Screen::Playing) {
        art_->processCompleted(2);
        updateScroll();
    }

    if (screen_ == Screen::Playing) {
        applyPendingSeek();
        updateVideoTexture();

        // The plan only exists once the server has answered, and the track
        // menu is built from it.
        if (player_ && playPlan_.sources.empty()) {
            JellyfinClient::PlaybackPlan plan = player_->plan();
            if (!plan.sources.empty()) {
                playPlan_ = plan;
                playSourceIndex_ = plan.sourceIndex;
                if (playAudioIndex_ < 0) playAudioIndex_ = plan.audioIndex;
                // Only adopt the server's subtitle default the first time,
                // so turning subtitles off does not undo itself on a seek.
                if (playSubtitleIndex_ < 0 && subtitleCues_.empty() &&
                    plan.subtitleIndex >= 0) {
                    playSubtitleIndex_ = plan.subtitleIndex;
                    loadSubtitleTrack(plan.subtitleIndex);
                }
            }
        }

        if (player_ && player_->state() == Player::State::Ended &&
            player_->queuedFrames() == 0) {
            handlePlaybackEnded();
        }

        // Tell the server where we are every so often, so resume works from
        // any client and a crash does not lose the position.
        const uint64_t now = Platform::NowMs();
        if (player_ && now - lastProgressReportMs_ > 10000) {
            lastProgressReportMs_ = now;
            const double position =
                player_->startOffsetSeconds() + player_->positionSeconds();
            const int64_t ticks = (int64_t)(position * 10000000.0);
            const std::string itemId = playingItem_.id;
            const bool paused = player_->paused();
            pool_.submit([this, itemId, ticks, paused] {
                client_.reportPlaybackProgress(itemId, ticks, paused);
            });
        }
    }

    if (scriptIndex_ < scriptedActions_.size() &&
        Platform::NowMs() >= nextScriptedMs_) {
        handleAction(scriptedActions_[scriptIndex_++]);
        nextScriptedMs_ = Platform::NowMs() + (uint64_t)scriptIntervalMs_;
    }

    if (screen_ == Screen::SignIn && !quickConnectCode_.empty()) {
        const uint64_t now = Platform::NowMs();
        if (now >= signInDeadlineMs_) {
            quickConnectCode_.clear();
            statusLine_ = "That code expired";
            errorLine_  = "Press A to get a new one";
        } else if (now >= nextPollMs_) {
            nextPollMs_ = now + kPollIntervalMs;
            pollQuickConnect();
        }
    }

    pollRequestQuickConnect();

    const uint64_t drawStart = Platform::NowMs();
    render_.beginFrame();
    draw();
    render_.trimTextCache();

    const uint64_t presentStart = Platform::NowMs();
    statDrawMs_ += (double)(presentStart - drawStart);

    if (!autoShotPath_.empty() && Platform::NowMs() >= autoShotAtMs_) {
        const bool ok = writeScreenshot(autoShotPath_);
        std::fprintf(stderr, "[shot] %s -> %s\n",
                     ok ? "wrote" : "FAILED", autoShotPath_.c_str());
        autoShotPath_.clear();
        running_ = false;
    }

    SDL_RenderPresent(sdl_);
    statPresentMs_ = (double)(Platform::NowMs() - presentStart);
    statPresentSumMs_ += statPresentMs_;
    ++statLoops_;

    return running_;
}

// --------------------------------------------------------------- drawing

void App::draw()
{
    render_.clear(Palette::Background);

    switch (screen_) {
        case Screen::Connecting:   drawConnecting();   break;
        case Screen::Welcome:      drawWelcome();      break;
        case Screen::ServerSelect: drawServerSelect(); break;
        case Screen::SignIn:       drawSignIn();       break;
        case Screen::Home:         drawHome();         break;
        case Screen::Settings:     drawSettings();     break;
        case Screen::About:        drawAbout();        break;
        case Screen::Browse:       drawBrowse();       break;
        case Screen::Detail:       drawDetail();       break;
        case Screen::Playing:      drawPlayer();       break;
    }

    drawChrome();
    drawToast();
    if (showDebug_ && settings_.diagnostics) drawDebugOverlay();
}

void App::drawChrome()
{
    // Playback owns the whole screen; its own controls replace this chrome.
    if (screen_ == Screen::Playing) return;

    // Top bar
    render_.fillRect({ 0, 0, logicalW_, kTopBarHeight }, Palette::Panel);
    render_.fillRect({ 0, kTopBarHeight - 2, logicalW_, 2 }, Palette::PanelHi);

    // The console's name first, in its own color, then the project's.
    int titleX = 40;
    const std::string platform = std::string(kPlatformName) + " ";
    render_.drawText(platform, titleX, 20, FontSize::Title, Palette::Platform);
    titleX += render_.textWidth(platform, FontSize::Title);
    render_.drawText("Butter", titleX, 20, FontSize::Title, Palette::Text);
    titleX += render_.textWidth("Butter", FontSize::Title);
    render_.drawText(" and Jelly", titleX, 20, FontSize::Title, Palette::Accent);

    if (client_.signedIn()) {
        render_.drawText(client_.userName(), logicalW_ - 40, 26,
                         FontSize::Body, Palette::TextDim, Align::Right);
    }

    // Bottom hint bar. Every screen tells the user which buttons do what,
    // because a TV app has no other affordance.
    const int hintY = logicalH_ - kHintBarHeight;
    render_.fillRect({ 0, hintY, logicalW_, kHintBarHeight }, Palette::Panel);
    render_.fillRect({ 0, hintY, logicalW_, 2 }, Palette::PanelHi);

    std::string hints;
    switch (screen_) {
        case Screen::Connecting:   hints = errorLine_.empty() ? "" : "$A  Retry"; break;
        case Screen::Welcome:      hints = "$A  Set this up"; break;
        case Screen::ServerSelect: hints = "$A  Connect      $Y  Search again"; break;
        case Screen::SignIn:       hints = "$B  Back"; break;
        case Screen::Browse:
            if (browseMenuOpen_) {
                hints = "$A  Select      Left/Right  Section      $B  Close";
            } else if (sidebarFocused_) {
                hints = onSearchScreen() ? "$A  Type a search"
                      : activeSource_ == MediaSource::Seerr ? "$A  Open"
                                                            : "$A  Open library";
            } else if (keyboardOpen_) {
                hints = "$A  Type      $Y  Delete      $B  Close keyboard";
            } else if (onSearchScreen() && items_.empty()) {
                hints = "$A  Type a search      $B  Menu";
            } else {
                const bool drillable = !items_.empty() &&
                    itemIndex_ < (int)items_.size() &&
                    (items_[itemIndex_].type == "Series" ||
                     items_[itemIndex_].type == "Season");
                hints  = drillable ? "$A  Open" : "$A  Details";
                hints += navStack_.empty() ? "      $B  Libraries" : "      $B  Back";
                if (curKind_ == LevelKind::Library) {
                    if (activeSource_ == MediaSource::Seerr) {
                        if (!discoverGenres_.empty()) hints += "      $X  Genre";
                    } else if (!curParentId_.empty()) {
                        hints += "      $X  Sort and filter";
                    }
                }
                hints += "      $Y  Refresh";
            }
            break;
        case Screen::Home:
            hints = sidebarFocused_ ? "$A  Open"
                                    : "$A  Details      $B  Menu      $Y  Refresh";
            break;
        case Screen::Settings:
            hints = sidebarFocused_ ? "$A  Open"
                                    : "$A  Select      $B  Menu";
            break;
        case Screen::About:
            hints = "$B  Back";
            break;
        case Screen::Detail:
            if (detailItem_.source == MediaSource::Seerr) {
                hints = detailItem_.requestStatus > 0
                    ? "$B  Back" : "$A  Request      $B  Back";
            } else {
                hints = detailItem_.partiallyWatched()
                    ? "$A  Resume      $Y  From the start      $X  Favorite      $S  Mark watched      $B  Back"
                    : "$A  Play      $X  Favorite      $S  Mark watched      $B  Back";
            }
            break;
        case Screen::Playing:
            hints = playerMenuOpen_
                ? "$A  Select      Left/Right  Section      $B  Close"
                : "$A  Pause      Left/Right  10s      $L/$R  1m      $X  Tracks      $B  Stop";
            break;
    }
    if (!hints.empty()) drawHints(hints, 40, hintY + 16);

    // On the hint bar rather than over the first row of posters.
    if (screen_ == Screen::Browse && !items_.empty() && !sidebarFocused_) {
        // A "+" where the server's count is not one the viewer can reach.
        const int loaded = (int)items_.size();
        const int total  = pageTotal_ > loaded ? pageTotal_ : loaded;
        const char* more = (pageTotal_ == 0 && pageNext_ > 0) ? "+" : "";

        char counter[80];
        std::snprintf(counter, sizeof(counter), "%d of %d%s%s",
                      itemIndex_ + 1, total, more,
                      pageLoading_ ? "   fetching more" : "");
        render_.drawText(counter, logicalW_ - 40, hintY + 16,
                         FontSize::Small, Palette::TextDim, Align::Right);
    }
}

void App::drawConnecting()
{
    const int cx = logicalW_ / 2;
    render_.drawText(statusLine_, cx, logicalH_ / 2 - 40,
                     FontSize::Title, Palette::Text, Align::Center);
    if (!errorLine_.empty()) {
        render_.drawText(errorLine_, cx, logicalH_ / 2 + 12,
                         FontSize::Body, Palette::Danger, Align::Center);
    }
}

// First run. Three services, and no reason to assume which one the viewer
// has, so ask instead of walking into Jellyfin discovery.
void App::drawWelcome()
{
    const int cx = logicalW_ / 2;

    render_.drawText("Add a server", cx, 190, FontSize::Huge,
                     Palette::Text, Align::Center);

    const int cardW = 340, cardH = 220;
    const SDL_Rect card = { cx - cardW / 2, 280, cardW, cardH };
    render_.fillRoundedRect(card, 12, Palette::PanelHi);
    render_.strokeRect(card, Palette::Accent, 3);

    const SDL_Rect iconBox = { cx - 44, card.y + 38, 88, 88 };
    if (iconJellyfin_) {
        Color tint = { 255, 255, 255, 255 };
        drawIcon(iconJellyfin_, iconBox, tint, false);
    }
    render_.drawText("Jellyfin", cx, card.y + 148, FontSize::Title,
                     Palette::Text, Align::Center);
}

void App::handleWelcomeAction(Action action)
{
    if (action == Action::Accept) beginDiscovery();
}

void App::drawServerSelect()
{
    const int cx = logicalW_ / 2;
    render_.drawText(statusLine_, cx, 130, FontSize::Title, Palette::Text, Align::Center);

    if (discovering_) {
        // A simple three-dot cycle reads as "working" without a sprite sheet.
        const int phase = (int)((Platform::NowMs() / 350) % 4);
        render_.drawText(std::string(phase, '.'), cx, 190,
                         FontSize::Huge, Palette::Accent, Align::Center);
        return;
    }

    if (servers_.empty()) {
        render_.drawTextWrapped(
            "Make sure the console and the server are on the same network. "
            "If the server is elsewhere, auto-discovery will not find it.",
            cx - 380, 200, 760, FontSize::Body, Palette::TextDim, 3);
        render_.drawText("Press A to search again", cx, 300,
                         FontSize::Body, Palette::Accent, Align::Center);
        return;
    }

    const int rowH  = 92;
    const int listW = 720;
    const int listX = cx - listW / 2;
    int y = 210;

    for (size_t i = 0; i < servers_.size(); ++i) {
        const bool focused = ((int)i == serverIndex_);
        const SDL_Rect row = { listX, y, listW, rowH - 12 };

        render_.fillRoundedRect(row, 10, focused ? Palette::PanelHi : Palette::Panel);
        if (focused) render_.strokeRect(row, Palette::Accent, 3);

        render_.drawTextClipped(servers_[i].name, listX + 26, y + 12, listW - 52,
                                FontSize::Title, Palette::Text);
        render_.drawTextClipped(servers_[i].address, listX + 26, y + 48, listW - 52,
                                FontSize::Small, Palette::TextDim);
        y += rowH;
    }
    // One row past the servers: type an address in by hand.
    if (!discovering_) {
        const int rowY = 250 + (int)servers_.size() * 96;
        const bool selected = (serverIndex_ >= (int)servers_.size());
        const SDL_Rect box = { logicalW_ / 2 - 360, rowY, 720, 74 };
        render_.fillRoundedRect(box, 10, selected ? Palette::PanelHi : Palette::Panel);
        if (selected) render_.strokeRect(box, Palette::Accent, 3);
        render_.drawText("Enter an address", box.x + 28, box.y + 22,
                         FontSize::Body,
                         selected ? Palette::Text : Palette::TextDim);
    }

}

void App::drawSignIn()
{
    const int cx = logicalW_ / 2;

    if (quickConnectCode_.empty()) {
        render_.drawText(statusLine_, cx, logicalH_ / 2 - 30,
                         FontSize::Title, Palette::Text, Align::Center);
        if (!errorLine_.empty()) {
            render_.drawText(errorLine_, cx, logicalH_ / 2 + 24,
                             FontSize::Body, Palette::Danger, Align::Center);
        }
        return;
    }

    render_.drawText("Quick Connect", cx, 120, FontSize::Title,
                     Palette::Text, Align::Center);

    render_.drawTextWrapped(
        "On any device, open Jellyfin, go to your user menu, choose "
        "Quick Connect, and enter this code:",
        cx - 400, 172, 800, FontSize::Body, Palette::TextDim, 2);

    // The code is the whole point of the screen, so it gets the space.
    const SDL_Rect codeBox = { cx - 260, 250, 520, 150 };
    render_.fillRoundedRect(codeBox, 16, Palette::Panel);
    render_.strokeRect(codeBox, Palette::Accent, 3);

    // Letter-spaced, because six characters read off a TV need the room.
    std::string spaced;
    for (size_t i = 0; i < quickConnectCode_.size(); ++i) {
        if (i) spaced += ' ';
        spaced += quickConnectCode_[i];
    }
    render_.drawText(spaced, cx, 272, FontSize::Display, Palette::Text, Align::Center);

    const int phase = (int)((Platform::NowMs() / 400) % 4);
    render_.drawText(statusLine_ + std::string(phase, '.'), cx, 430,
                     FontSize::Body, Palette::TextDim, Align::Center);

    if (!signInServerName_.empty()) {
        render_.drawText(signInServerName_ + "  -  " + client_.serverUrl(),
                         cx, 470, FontSize::Small, Palette::TextDim, Align::Center);
    }

    const uint64_t now = Platform::NowMs();
    if (signInDeadlineMs_ > now) {
        const int secondsLeft = (int)((signInDeadlineMs_ - now) / 1000);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Expires in %d:%02d",
                      secondsLeft / 60, secondsLeft % 60);
        render_.drawText(buf, cx, 506, FontSize::Small, Palette::TextDim, Align::Center);
    }
}

void App::drawSidebar()
{
    render_.fillRect({ 0, kTopBarHeight, kSidebarWidth,
                       logicalH_ - kTopBarHeight - kHintBarHeight }, Palette::Panel);

    drawBottomBar();

    const int listTop    = kTopBarHeight + 24;
    const int listBottom = logicalH_ - kHintBarHeight - kBottomBarHeight;

    // Keep the selected entry on screen. Entries are a fixed pitch, so this
    // is a first-visible index rather than a pixel offset. The LIBRARIES
    // heading costs a row's worth of space and has to be counted, or the
    // last entry is clipped while the scroll believes everything fits.
    const int rowPitch    = 62;
    const int headingCost = 38;
    // Every heading costs a row's worth of space. Counting them is what
    // stops the last entry being clipped while the scroll believes it fits.
    int headings = 0;
    for (size_t i = 0; i < sidebar_.size(); ++i) {
        const SidebarEntry::Kind kind = sidebar_[i].kind;
        if (kind != SidebarEntry::Kind::Library &&
            kind != SidebarEntry::Kind::Discover &&
            kind != SidebarEntry::Kind::Requests) continue;
        if (i == 0 || sidebar_[i - 1].kind != kind) ++headings;
    }
    const int usable  = listBottom - listTop - headings * headingCost;
    const int visible = usable / rowPitch;
    if (visible > 0) {
        if (libraryIndex_ < sidebarScroll_) sidebarScroll_ = libraryIndex_;
        if (libraryIndex_ > sidebarScroll_ + visible - 1) {
            sidebarScroll_ = libraryIndex_ - visible + 1;
        }
        const int maxScroll = (int)sidebar_.size() - visible;
        if (sidebarScroll_ > maxScroll) sidebarScroll_ = maxScroll;
        if (sidebarScroll_ < 0) sidebarScroll_ = 0;
    }

    int y = listTop;

    for (size_t i = (size_t)sidebarScroll_; i < sidebar_.size(); ++i) {
        // A heading wherever the kind changes, so groups read as groups.
        const SidebarEntry::Kind kind = sidebar_[i].kind;
        const bool startsGroup = (i == (size_t)sidebarScroll_) ||
                                 sidebar_[i - 1].kind != kind;
        const char* heading = nullptr;
        if (startsGroup) {
            if      (kind == SidebarEntry::Kind::Library)  heading = "LIBRARIES";
            else if (kind == SidebarEntry::Kind::Discover) heading = "DISCOVER";
            else if (kind == SidebarEntry::Kind::Requests) heading = "REQUESTS";
        }
        if (heading) {
            render_.drawText(heading, 32, y + 6, FontSize::Small, Palette::TextDim);
            y += 38;
        }

        const bool selected = ((int)i == libraryIndex_);

        // Stop before the fixed bar rather than drawing behind it.
        if (y + 54 > listBottom) break;

        const SDL_Rect row = { 20, y, kSidebarWidth - 40, 54 };

        if (selected) {
            render_.fillRoundedRect(row, 8,
                (sidebarFocused_ && !bottomBarFocused_) ? Palette::Accent
                                                       : Palette::PanelHi);
        }
        const Color label = selected ? Palette::Text : Palette::TextDim;
        render_.drawTextClipped(sidebar_[i].title, 38, y + 13,
                                kSidebarWidth - 76, FontSize::Body, label);
        y += 62;
    }
}

// The bar's icons, loaded once. Missing files are not fatal: the bar falls
// back to text, which is what it drew before there were any.
void App::loadBarIcons()
{
    const std::string dir = Platform::AssetDir() + "/icons/";
    struct { const char* file; SDL_Texture** into; } wanted[] = {
        { "jellyfin.png", &iconJellyfin_ },
        { "seerr.png",    &iconSeerr_    },
        { "gear.png",     &iconGear_     },
    };
    for (auto& w : wanted) {
        *w.into = Image::LoadTexture(sdl_, dir + w.file);
        if (!*w.into) LOGF("[ui] no icon %s", w.file);
    }
}

// Centered, square, and tinted. The gear ships white so this can color it;
// the service marks are their own artwork and only get faded.
void App::drawIcon(SDL_Texture* icon, const SDL_Rect& box, Color tint,
                   bool recolor)
{
    if (!icon) return;

    const int side = (box.w < box.h ? box.w : box.h) - 16;
    if (side <= 0) return;
    const SDL_Rect dst = { box.x + (box.w - side) / 2,
                           box.y + (box.h - side) / 2, side, side };

    SDL_SetTextureColorMod(icon, recolor ? tint.r : 255,
                                  recolor ? tint.g : 255,
                                  recolor ? tint.b : 255);
    SDL_SetTextureAlphaMod(icon, tint.a);
    SDL_RenderCopy(sdl_, icon, nullptr, &dst);
    SDL_SetTextureColorMod(icon, 255, 255, 255);
    SDL_SetTextureAlphaMod(icon, 255);
}

// The server the active source is pointed at.
std::string App::activeServerName() const
{
    switch (activeSource_) {
        case MediaSource::Seerr: {
            std::string url = seerr_.baseUrl();
            if (url.rfind("http://", 0) == 0) url = url.substr(7);
            return url.empty() ? "Not configured" : url;
        }
        case MediaSource::Jellyfin:
        default: {
            std::string url = client_.serverUrl();
            if (url.rfind("http://", 0) == 0) url = url.substr(7);
            return url.empty() ? "Not connected" : url;
        }
    }
}

// Which server the active service is pointed at, with the service's own mark
// beside it. Opening it picks a different one.
void App::drawServerRow(int y)
{
    const bool selected = bottomBarFocused_ && bottomRow_ == 0;
    const SDL_Rect box  = { 12, y + 5, kSidebarWidth - 24, kServerRowHeight - 8 };

    if (selected) render_.fillRoundedRect(box, 6, Palette::PanelHi);

    SDL_Texture* mark = nullptr;
    switch (activeSource_) {
        case MediaSource::Jellyfin: mark = iconJellyfin_; break;
        case MediaSource::Seerr:    mark = iconSeerr_;    break;
    }

    const SDL_Rect markBox = { box.x + 2, box.y, 30, box.h };
    if (mark) {
        Color tint = { 255, 255, 255, 255 };
        drawIcon(mark, markBox, tint, false);
    }

    const int textX = markBox.x + markBox.w + 8;
    render_.drawTextClipped(activeServerName(), textX,
                            box.y + box.h / 2 - render_.lineHeight(FontSize::Small) / 2,
                            box.w - (textX - box.x) - 10, FontSize::Small,
                            selected ? Palette::Text : Palette::TextDim);
}

// The bar along the bottom of the sidebar: service tabs, then the gear.
void App::drawBottomBar()
{
    const int barY = logicalH_ - kHintBarHeight - kBottomBarHeight;
    render_.fillRect({ 0, barY, kSidebarWidth, kBottomBarHeight }, Palette::Panel);
    render_.fillRect({ 0, barY, kSidebarWidth, 2 }, Palette::PanelHi);

    drawServerRow(barY);

    const int y = barY + kServerRowHeight;
    const int gap      = 10;
    const int tabsWide = kSidebarWidth - 40 - kGearWidth - gap;
    const int width    = (tabsWide - gap * (kServiceTabCount - 1)) / kServiceTabCount;
    const int boxY     = y + 8;
    const int boxH     = kTabRowHeight - 16;

    for (int i = 0; i < kServiceTabCount; ++i) {
        const ServiceTab& tab = kServiceTabs[i];
        const bool active   = (tab.source == activeSource_);
        const bool selected = bottomBarFocused_ && bottomRow_ == 1 &&
                              i == bottomTab_;
        const bool usable = (tab.source == MediaSource::Jellyfin)
                          ? client_.signedIn() : seerr_.configured();

        const SDL_Rect box = { 20 + i * (width + gap), boxY, width, boxH };
        render_.fillRoundedRect(box, 8, active ? Palette::PanelHi : Palette::Panel);

        // Full strength for the service in use, half for the other, a quarter
        // for one that has no server to talk to.
        Uint8 alpha = 255;
        if (!active) alpha = 130;
        if (!usable) alpha = 70;

        SDL_Texture* icon = (tab.source == MediaSource::Jellyfin) ? iconJellyfin_
                                                                  : iconSeerr_;
        if (icon) {
            Color tint = { 255, 255, 255, alpha };
            drawIcon(icon, box, tint, false);
        } else {
            Color color = tab.color;
            color.a = alpha;
            render_.drawText(tab.label, box.x + box.w / 2,
                             box.y + boxH / 2 - render_.lineHeight(FontSize::Small) / 2,
                             FontSize::Small, color, Align::Center);
        }

        if (selected) render_.strokeRect(box, Palette::Accent, 3);
    }

    const SDL_Rect gear = { kSidebarWidth - 20 - kGearWidth, boxY,
                            kGearWidth, boxH };
    const bool gearSelected = bottomBarFocused_ && bottomRow_ == 1 &&
                              bottomTab_ == kServiceTabCount;
    // Lit while the settings screen is up, not only while the stick is on it.
    const bool gearActive = (screen_ == Screen::Settings);
    render_.fillRoundedRect(gear, 8,
                            (gearSelected || gearActive) ? Palette::PanelHi
                                                         : Palette::Panel);
    if (iconGear_) {
        Color tint = (gearSelected || gearActive) ? Palette::Text : Palette::TextDim;
        tint.a = (gearSelected || gearActive) ? 255 : 150;
        drawIcon(iconGear_, gear, tint, true);
    }
    if (gearSelected) render_.strokeRect(gear, Palette::Accent, 3);
}

// Draws a poster with its label, and a progress bar when the item has been
// started. Shared by the grid and the home rows.
// Swapping the glyph font is a file open, so only do it on a real change.
void App::useInputScheme(InputScheme scheme)
{
    if (scheme == inputScheme_) return;
    inputScheme_ = scheme;
    render_.setIconFont(fontDir_ + "/" + HintFontFile(scheme));
}

// Walks a hint string, drawing labels in the text face and $ placeholders in
// the glyph face. Falls back to words if the glyph font did not load.
void App::drawHints(const std::string& text, int x, int y)
{
    const Color c = Palette::TextDim;

    if (!render_.hasIconFont()) {
        render_.drawText(ExpandHints(text, inputScheme_), x, y,
                         FontSize::Small, c);
        return;
    }

    // Centered against the text line rather than sharing its baseline: the
    // glyph box is square and taller than the words beside it.
    const int glyphY = y + (render_.lineHeight(FontSize::Small)
                            - render_.iconHeight()) / 2;

    std::string run;
    int cursor = x;
    auto flush = [&] {
        if (run.empty()) return;
        render_.drawText(run, cursor, y, FontSize::Small, c);
        cursor += render_.textWidth(run, FontSize::Small);
        run.clear();
    };

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '$' || i + 1 >= text.size()) { run += text[i]; continue; }

        const std::string glyph = HintGlyph(inputScheme_, text[i + 1]);
        if (glyph.empty()) { run += text[i]; continue; }
        ++i;

        flush();
        render_.drawIcon(glyph, cursor, glyphY, c);
        cursor += render_.iconWidth(glyph);
    }
    flush();
}

void App::drawTile(const JfItem& item, const SDL_Rect& tile, bool focused)
{
    render_.fillRoundedRect(tile, 6, Palette::Panel);

    // Ask for exactly the box it will be drawn into, so the server does the
    // cropping and scaling and this ends up a 1:1 blit.
    SDL_Texture* poster = art_->get(item.id, item.primaryTag, tile.w, tile.h,
                                    item.artUrl);
    if (poster) {
        render_.drawTextureCover(poster, tile);
    } else {
        render_.drawTextClipped(item.name, tile.x + 10, tile.y + tile.h / 2 - 12,
                                tile.w - 20, FontSize::Small, Palette::TextDim);
    }

    // What a request is doing, which is not a thing watch state can say.
    if (item.requestStatus > 0) {
        const char* label = RequestStatusLabel(item.requestStatus);
        if (label[0]) {
            const int pad = 8;
            const int w = render_.textWidth(label, FontSize::Small) + pad * 2;
            const int h = 24;
            const SDL_Rect badge = { tile.x + 8, tile.y + tile.h - h - 8, w, h };
            render_.fillRoundedRect(badge, 6,
                item.requestStatus == kRequestAvailable ? Palette::AccentWarm
                                                        : Palette::Accent);
            render_.drawText(label, badge.x + pad, badge.y + 4,
                             FontSize::Small, Palette::Text);
        }
    }

    // A genre tile is a backdrop with no title in it, so the name goes on.
    if (item.type == "Genre") {
        render_.fillRect({ tile.x, tile.y + tile.h / 2 - 20, tile.w, 40 },
                         Palette::Shadow);
        render_.drawTextClipped(item.name, tile.x + tile.w / 2,
                                tile.y + tile.h / 2 - 12, tile.w - 16,
                                FontSize::Body, Palette::Text, Align::Center);
        if (focused) render_.strokeRect(tile, Palette::Accent, 4);
        return;
    }

    // How far in, drawn over the bottom of the artwork where it reads at a
    // glance without needing a number.
    if (item.partiallyWatched()) {
        const int barHeight = 5;
        SDL_Rect track = { tile.x, tile.y + tile.h - barHeight, tile.w, barHeight };
        render_.fillRect(track, Palette::Shadow);
        track.w = (int)(tile.w * std::min(1.0, std::max(0.0, item.playedFraction)));
        render_.fillRect(track, Palette::Accent);
    } else if (item.played) {
        const SDL_Rect dot = { tile.x + tile.w - 22, tile.y + 8, 14, 14 };
        render_.fillRoundedRect(dot, 7, Palette::Accent);
    }

    // Top left, so it never lands on the watched dot.
    if (item.isFavorite) {
        const SDL_Rect mark = { tile.x + 8, tile.y + 8, 6, 16 };
        render_.fillRoundedRect(mark, 3, Palette::AccentWarm);
    }

    if (focused) render_.strokeRect(tile, Palette::Accent, 4);
}

int App::homeColumns() const
{
    const int available = logicalW_ - kSidebarWidth - 80;
    // Around 200 logical pixels a tile: wide enough for a poster to read
    // across a room, narrow enough that a large window shows more of the
    // library rather than the same four things larger.
    int columns = available / 220;
    if (columns < 3) columns = 3;
    if (columns > BJ_MAX_GRID_COLUMNS) columns = BJ_MAX_GRID_COLUMNS;
    return columns;
}

int App::homeRowHeight(size_t row) const
{
    if (row >= homeRows_.size()) return 0;
    const int columns   = homeColumns();
    const int available = logicalW_ - kSidebarWidth - 80;
    const int tileWidth = (available - 20 * (columns - 1)) / columns;
    const int tileHeight = RowHasEpisodes(homeRows_[row]) ? (tileWidth * 9) / 16
                                                          : (tileWidth * 3) / 2;
    return tileHeight + kHomeRowChrome;
}

void App::clampHomeScroll()
{
    const int bodyHeight = logicalH_ - kTopBarHeight - kHintBarHeight - 16;

    int top = 0;
    for (int r = 0; r < homeRow_ && r < (int)homeRows_.size(); ++r) {
        top += homeRowHeight((size_t)r);
    }
    const int height = homeRowHeight((size_t)homeRow_);

    float target = homeScrollTargetPx_;
    if ((float)top < target) target = (float)top;
    if ((float)(top + height) > target + bodyHeight) {
        target = (float)(top + height - bodyHeight);
    }

    int total = 0;
    for (size_t r = 0; r < homeRows_.size(); ++r) total += homeRowHeight(r);
    const float maxScroll = (float)(total > bodyHeight ? total - bodyHeight : 0);
    if (target > maxScroll) target = maxScroll;
    if (target < 0.0f)      target = 0.0f;
    homeScrollTargetPx_ = target;
}

void App::drawHome()
{
    drawSidebar();

    if (homeLoading_) {
        render_.drawText("Loading...", kSidebarWidth + 40, kTopBarHeight + 40,
                         FontSize::Body, Palette::TextDim);
        return;
    }
    if (homeRows_.empty()) {
        render_.drawText("Nothing to continue yet", kSidebarWidth + 40,
                         kTopBarHeight + 40, FontSize::Body, Palette::TextDim);
        return;
    }

    const int left      = kSidebarWidth + 40;
    const int available = logicalW_ - left - 40;
    const int columns   = homeColumns();
    const int gap       = 20;
    const int tileWidth = (available - gap * (columns - 1)) / columns;

    const int bodyTop    = kTopBarHeight + 16;
    const int bodyBottom = logicalH_ - kHintBarHeight;
    // Rows are taller than the screen once there are more than two of them,
    // so the body scrolls and a row on the edge is clipped rather than
    // dropped: seeing half of the next one is how you know it is there.
    const SDL_Rect clip = { left - 20, bodyTop, logicalW_ - left + 20,
                            bodyBottom - bodyTop };
    SDL_RenderSetClipRect(sdl_, &clip);

    int y = bodyTop - (int)(homeScrollPx_ + 0.5f);
    for (size_t r = 0; r < homeRows_.size(); ++r) {
        const HomeRow& row = homeRows_[r];

        // An episode still in a poster frame looks wrong, and a film in a
        // 16:9 one looks like a screenshot, so the row follows its contents.
        const bool wideRow = RowHasEpisodes(row);
        const int tileHeight = wideRow ? (tileWidth * 9) / 16
                                       : (tileWidth * 3) / 2;
        const int rowHeight = tileHeight + kHomeRowChrome;
        if (y + rowHeight < bodyTop) { y += rowHeight; continue; }
        if (y > bodyBottom) break;

        const bool rowFocused = (!sidebarFocused_ && (int)r == homeRow_);

        render_.drawText(row.title, left, y,
                         FontSize::Body,
                         rowFocused ? Palette::Text : Palette::TextDim);
        const int tileTop = y + 34;

        for (int i = 0; i < columns; ++i) {
            const int index = row.scroll + i;
            if (index >= (int)row.items.size()) break;

            const SDL_Rect tile = { left + i * (tileWidth + gap), tileTop,
                                    tileWidth, tileHeight };
            const bool focused = rowFocused && index == row.focus;
            drawTile(row.items[index], tile, focused);

            // Two lines rather than one clipped one: "Series - Episode" on a
            // single line loses the episode, which is the half that matters.
            const JfItem& item = row.items[index];
            const Color labelColor = focused ? Palette::Text : Palette::TextDim;
            std::string primary = item.name;
            std::string second;
            if (!item.seriesName.empty()) {
                primary = item.seriesName;
                second  = item.name;
            } else if (item.productionYear > 0) {
                second = YearText(item.productionYear);
                const char* kind = (item.source == MediaSource::Seerr)
                                 ? MediaKindLabel(item) : "";
                if (kind[0]) second += std::string("   ") + kind;
            }

            render_.drawTextClipped(primary, tile.x, tile.y + tile.h + 8,
                                    tileWidth, FontSize::Small, labelColor);
            if (!second.empty()) {
                render_.drawTextClipped(second, tile.x, tile.y + tile.h + 30,
                                        tileWidth, FontSize::Small,
                                        Palette::TextDim);
            }
        }
        y += rowHeight;
    }

    SDL_RenderSetClipRect(sdl_, nullptr);

    // Scroll bar, so a long discovery page does not feel bottomless.
    int total = 0;
    for (size_t r = 0; r < homeRows_.size(); ++r) total += homeRowHeight(r);
    const int bodyHeight = bodyBottom - bodyTop;
    if (total > bodyHeight) {
        const int trackX = logicalW_ - 22;
        render_.fillRoundedRect({ trackX, bodyTop, 5, bodyHeight }, 2, Palette::Panel);

        const int thumbH = std::max(40, bodyHeight * bodyHeight / total);
        const int travel = bodyHeight - thumbH;
        const int maxScroll = total - bodyHeight;
        const int thumbY = bodyTop +
            (maxScroll > 0 ? (int)(travel * homeScrollPx_ / maxScroll) : 0);
        render_.fillRoundedRect({ trackX, thumbY, 5, thumbH }, 2, Palette::Accent);
    }
}

std::vector<App::SettingsEntry> App::settingsRows() const
{
    std::vector<SettingsEntry> rows;
    auto heading = [&rows](const char* title) {
        rows.push_back({ SettingAction::Heading, title, "", "", false, -1 });
    };

    std::string serverName = client_.serverUrl();
    if (serverName.rfind("http://", 0) == 0) serverName = serverName.substr(7);

    heading("JELLYFIN");
    rows.push_back({ SettingAction::JellyfinServer, "Server",
                     serverName.empty() ? "not set" : serverName,
                     "Switch", false, -1 });
    rows.push_back({ SettingAction::JellyfinAccount, "Account",
                     client_.userName().empty() ? "not signed in" : client_.userName(),
                     (confirmSignOut_ && Platform::NowMs() < confirmUntilMs_)
                         ? "Press A again" : "Sign out", false, -1 });

    heading("SEERR");
    const bool manual = settings_.requestServerElsewhere;
    rows.push_back({ SettingAction::RequestAlongside, "Automatic",
                     manual ? "beside Jellyfin, signed in with the same account"
                            : requestServerSummary(),
                     "", false, manual ? 0 : 1 });
    rows.push_back({ SettingAction::RequestElsewhere, "Manual configuration",
                     manual ? requestServerSummary()
                            : "another machine, or a different account",
                     "", false, manual ? 1 : 0 });

    if (manual) {
        rows.push_back({ SettingAction::RequestAddress, "Address",
                         seerrFormAddress_.empty() ? "not set" : seerrFormAddress_,
                         "Change", true, -1 });
        rows.push_back({ SettingAction::RequestUser, "User name",
                         seerrFormUser_.empty() ? "not set" : seerrFormUser_,
                         "Change", true, -1 });
        rows.push_back({ SettingAction::RequestPassword, "Password",
                         seerrFormPassword_.empty()
                             ? "not set" : std::string(seerrFormPassword_.size(), '*'),
                         "Change", true, -1 });
        rows.push_back({ SettingAction::RequestTest, "Check it works",
                         seerr_.signedIn() ? "signed in as " + seerr_.userName()
                                           : "nothing signed in yet",
                         "Test", true, -1 });
        rows.push_back({ SettingAction::RequestSave, "Keep it",
                         "signs in and remembers", "Save", true, -1 });
    }

    heading("VIDEO");
    int height = settings_.playbackHeight;
    if (kPlaybackMaxHeight > 0 && (height <= 0 || height > kPlaybackMaxHeight)) {
        height = kPlaybackMaxHeight;
    }
    rows.push_back({ SettingAction::Playback, "Quality",
                     QualityLabel(height), "Change", false, -1 });
    rows.push_back({ SettingAction::VideoBitrate, "Bitrate",
                     VideoBitrateLabel(settings_.videoBitrate),
                     "Change", false, -1 });
    rows.push_back({ SettingAction::Framerate, "Frame rate",
                     FramerateLabel(settings_.maxFramerate),
                     "Change", false, -1 });
#if BJ_HAS_SECOND_SCREEN
    rows.push_back({ SettingAction::Display, "Screens",
                     DisplayLabel(settings_.display), "Change", false, -1 });
#endif

    heading("AUDIO");
    rows.push_back({ SettingAction::AudioFormat, "Format",
                     AudioFormatLabel(settings_.audioFormat),
                     BJ_AUDIO_AAC ? "Change" : "", false, -1 });
    rows.push_back({ SettingAction::AudioBitrate, "Bitrate",
                     bj::ToString(settings_.audioBitrate) + " kbit/s",
                     "Change", false, -1 });

    heading("DIAGNOSTICS");
    rows.push_back({ SettingAction::Diagnostics, "Playback timings",
                     settings_.diagnostics
                         ? "on: writing decode and draw times to log.txt"
                         : "off",
                     "", false, settings_.diagnostics ? 1 : 0 });
    rows.push_back({ SettingAction::DiagnosticsNote, "",
                     "Measures how long each frame takes and writes it to "
                     "log.txt on this device, with the titles being played. "
                     "Nothing is sent anywhere. Leave it off unless you are "
                     "chasing a problem.", "", true, -1 });

    heading("ABOUT");
    rows.push_back({ SettingAction::About, std::string(kAppName),
                     "Version " + std::string(kAppVersion), "Credits", false, -1 });
    return rows;
}

int App::nextSettingsRow(const std::vector<SettingsEntry>& rows,
                         int from, int step) const
{
    for (int at = from + step; at >= 0 && at < (int)rows.size(); at += step) {
        const SettingAction what = rows[(size_t)at].action;
        if (what != SettingAction::Heading &&
            what != SettingAction::DiagnosticsNote) {
            return at;
        }
    }
    return from;
}


void App::drawSettings()
{
    drawSidebar();

    // Typing an address takes the whole pane: the list behind a keyboard is
    // unreadable and there is nothing to read on it anyway.
    if (keyboardOpen_ && keyboardFor_ != KeyboardFor::Search) {
        drawSearch();
        drawKeyboard();
        return;
    }

    const int left  = kSidebarWidth + 48;
    const int width = logicalW_ - left - 48;
    int y = kTopBarHeight + 28;

    render_.drawText("Settings", left, y, FontSize::Title, Palette::Text);
    y += 62;

    const std::vector<SettingsEntry> rows = settingsRows();
    const int count = (int)rows.size();
    if (settingsRow_ >= count) settingsRow_ = count - 1;
    if (settingsRow_ < 0) settingsRow_ = 0;

    // Same trick as the sidebar: the rows are a fixed pitch, so keeping the
    // selected one on screen is a first-visible index.
    const int bottom = logicalH_ - kHintBarHeight - 12;
    auto rowHeight = [&rows](int at) {
        switch (rows[(size_t)at].action) {
            case SettingAction::Heading:         return 44;
            case SettingAction::DiagnosticsNote: return 96;
            default:                             return 86;
        }
    };

    // Headings make the rows uneven, so the first visible one is found by
    // walking up from the selection until it fits rather than by division.
    if (settingsRow_ < settingsScroll_) settingsScroll_ = settingsRow_;
    for (;;) {
        int used = 0;
        for (int at = settingsScroll_; at <= settingsRow_; ++at) used += rowHeight(at);
        if (used <= bottom - y || settingsScroll_ >= settingsRow_) break;
        ++settingsScroll_;
    }
    if (settingsScroll_ < 0) settingsScroll_ = 0;

    for (int i = settingsScroll_; i < count; ++i) {
        // A heading and its rule, which nothing lands on.
        // Explanatory text, wrapped, with no box and nothing to select.
        if (rows[(size_t)i].action == SettingAction::DiagnosticsNote) {
            const int noteWidth = width - 56;
            const int used = render_.drawTextWrapped(rows[(size_t)i].value,
                                                     left + 28, y + 4, noteWidth,
                                                     FontSize::Small,
                                                     Palette::TextDim, 4);
            y += used + 18;
            continue;
        }

        if (rows[(size_t)i].action == SettingAction::Heading) {
            if (y + 44 > bottom) break;
            render_.drawText(rows[(size_t)i].label, left, y + 12,
                             FontSize::Small, Palette::Accent);
            const int textWidth = render_.textWidth(rows[(size_t)i].label,
                                                    FontSize::Small);
            render_.fillRect({ left + textWidth + 16, y + 22,
                               width - textWidth - 16, 1 }, Palette::PanelHi);
            y += 44;
            continue;
        }

        if (y + 74 > bottom) break;
        const bool focused = (!sidebarFocused_ && i == settingsRow_);

        // A row belonging to the form is inset, so it reads as part of the
        // choice above it rather than another setting.
        const int inset = rows[i].indented ? 28 : 0;
        const SDL_Rect box = { left + inset, y, width - inset, 74 };
        render_.fillRoundedRect(box, 8, focused ? Palette::PanelHi : Palette::Panel);
        if (focused) render_.strokeRect(box, Palette::Accent, 3);

        const int textX = box.x + 24;
        if (!rows[i].label.empty()) {
            render_.drawText(rows[i].label, textX, y + 12, FontSize::Body,
                             focused ? Palette::Text : Palette::TextDim);
            render_.drawTextClipped(rows[i].value, textX, y + 40, box.w - 220,
                                    FontSize::Small, Palette::TextDim);
        } else {
            // A row that is only its action, centred against the box.
            render_.drawTextClipped(rows[i].value, textX, y + 26, box.w - 220,
                                    FontSize::Small, Palette::TextDim);
        }
        if (rows[i].radio >= 0) {
            // A ring with the background punched out of it, and a dot in
            // the middle for the option in force.
            const int size = 24;
            const int rx = box.x + box.w - 24 - size;
            const int ry = y + (74 - size) / 2;
            const Color ring = focused ? Palette::Accent : Palette::TextDim;
            render_.fillRoundedRect({ rx, ry, size, size }, size / 2, ring);
            render_.fillRoundedRect({ rx + 3, ry + 3, size - 6, size - 6 },
                                    (size - 6) / 2,
                                    focused ? Palette::PanelHi : Palette::Panel);
            if (rows[i].radio == 1) {
                render_.fillRoundedRect({ rx + 7, ry + 7, size - 14, size - 14 },
                                        (size - 14) / 2, Palette::Accent);
            }
        } else {
            render_.drawText(rows[i].hint, box.x + box.w - 24, y + 24,
                             FontSize::Small,
                             focused ? Palette::Accent : Palette::TextDim,
                             Align::Right);
        }
        y += 86;
    }

    // Typing an address, then waiting on the code, both happen over the top
    // of the list rather than on a screen of their own.

    if (!seerrCode_.empty()) {
        const SDL_Rect panel = { left, kTopBarHeight + 28, width,
                                 logicalH_ - kTopBarHeight - kHintBarHeight - 60 };
        render_.fillRoundedRect(panel, 12, Palette::Panel);

        const int cx = panel.x + panel.w / 2;
        render_.drawText("Approve this code in Jellyfin", cx, panel.y + 40,
                         FontSize::Body, Palette::TextDim, Align::Center);
        render_.drawText(seerrCode_, cx, panel.y + 90, FontSize::Huge,
                         Palette::Accent, Align::Center);
        render_.drawTextWrapped(
            "Open Jellyfin, go to your user menu and choose Quick Connect. "
            "The request server signs in as the same account, so there is no "
            "key to type.",
            panel.x + 40, panel.y + 190, panel.w - 80, FontSize::Small,
            Palette::TextDim, 3);
    }
}

void App::drawAbout()
{
    render_.clear(Palette::Background);

    const int cx = logicalW_ / 2;

    struct Line { const char* text; bool dim; };
    const Line lines[] = {
        { "Built with", true },
#if defined(__WIIU__)
        { "devkitPro and wut  -  toolchain and Cafe OS headers", false },
        { "SDL2 for Wii U  -  windowing, input, audio", false },
#elif defined(_XENON)
        { "OXDK  -  toolchain, XEX2 and the Xenon ABI", false },
        { "SDL2x360  -  windowing, input, audio", false },
#else
        { "SDL2  -  windowing, input, audio", false },
#endif
#if defined(_XENON)
        // Different libraries under here, and they deserve the credit the
        // ones they stand in for get on the other console.
        { "FFmpeg  -  H.264 decoding", false },
        { "minimp3  -  MP3 decoding", false },
        { "stb_truetype and stb_image  -  text and artwork", false },
#else
        { "mpg123  -  MP3 decoding", false },
        { "SDL_ttf and SDL_image  -  text and artwork", false },
#endif
        { "cJSON  -  JSON parsing", false },
        { "Noto Sans  -  SIL Open Font License", false },
        { "", true },
        { "With thanks to", true },
#if defined(__WIIU__)
        { "GaryOderNichts  -  wiiu-shaders and FFmpeg-wiiu", false },
        { "decaf-emu  -  latte-assembler", false },
#elif defined(_XENON)
        { "Wolf3s  -  SDL2x360", false },
        { "Sean Barrett  -  the stb libraries", false },
        { "lieff  -  minimp3", false },
        { "The Free60 and libxenon projects", false },
#endif
        { "The Jellyfin project", false },
        { "The Seerr project", false },
        { "Kenney  -  the input prompt glyphs", false },
    };

    // The credits are as long as the platform's dependency list, and the
    // screen is as short as 480 lines, so the pitch follows what is left
    // rather than being fixed. The margin keeps it out of a TV's overscan.
    const int top    = kTopBarHeight + 12;
    const int margin = kHintBarHeight + 12;
    const int count  = (int)(sizeof(lines) / sizeof(lines[0]));

    const std::string version = std::string("Version ") + kAppVersion +
                                "  -  a Jellyfin client for " + kPlatformArticleName;
    const int titleHeight = render_.lineHeight(FontSize::Huge);
    const int bodyHeight  = render_.lineHeight(FontSize::Body);
    const int smallHeight = render_.lineHeight(FontSize::Small);

    const int headHeight = titleHeight + bodyHeight * 2 + smallHeight * 2 + 24;
    const int forCredits = logicalH_ - top - margin - headHeight;
    int pitch = forCredits / (count > 0 ? count : 1);
    if (pitch > 30) pitch = 30;
    if (pitch < smallHeight) pitch = smallHeight;

    int y = top;
    render_.drawText("Butter and Jelly", cx, y, FontSize::Huge,
                     Palette::Text, Align::Center);
    y += titleHeight + 6;
    render_.drawText(version, cx, y, FontSize::Body, Palette::TextDim,
                     Align::Center);
    y += bodyHeight + 6;
    render_.drawText("Built with love in Cleveland", cx, y, FontSize::Body,
                     Palette::Platform, Align::Center);
    y += bodyHeight + 4;
    render_.drawText("Copyright (C) " BJ_COPYRIGHT_YEAR " Horrible Dev",
                     cx, y, FontSize::Small, Palette::TextDim, Align::Center);
    y += smallHeight + 2;
    render_.drawText("https://horrible.dev", cx, y, FontSize::Small,
                     Palette::AccentWarm, Align::Center);
    y += smallHeight + 12;

    // On a 480 line screen the list does not fit however tight the pitch, so
    // it splits into two columns rather than running off the bottom.
    const int rows = (pitch * count <= forCredits) ? count : (count + 1) / 2;
    const int columnCentres[2] = { rows == count ? cx : logicalW_ / 4,
                                   rows == count ? cx : (logicalW_ * 3) / 4 };

    const int creditsTop = y;
    for (int i = 0; i < count; ++i) {
        if (!lines[i].text[0]) continue;
        const int column = i / rows;
        render_.drawText(lines[i].text, columnCentres[column],
                         creditsTop + (i % rows) * pitch, FontSize::Small,
                         lines[i].dim ? Palette::TextDim : Palette::Text,
                         Align::Center);
    }
}

void App::drawSearch()
{
    const int left = kSidebarWidth + 40;
    const SDL_Rect box = { left, kTopBarHeight + 16, logicalW_ - left - 40, 56 };
    render_.fillRoundedRect(box, 8, Palette::Panel);
    render_.strokeRect(box, Palette::Accent, 2);

    const char* empty = "Press A to search";
    if (keyboardOpen_) {
        switch (keyboardFor_) {
            case KeyboardFor::RequestServerAddress:
                empty = "Address of your Seerr server"; break;
            case KeyboardFor::RequestUser:
                empty = "Your Seerr user name"; break;
            case KeyboardFor::RequestPassword:
                empty = "Password"; break;
            case KeyboardFor::ServerAddress:
                empty = "Address of your Jellyfin server"; break;
            case KeyboardFor::Search:
                empty = "Pick letters with A"; break;
        }
    }
    std::string typed = searchTerm_;
    if (keyboardFor_ == KeyboardFor::RequestPassword) {
        typed.assign(searchTerm_.size(), '*');
    }
    const std::string shown = !typed.empty() ? typed : empty;
    render_.drawTextClipped(shown, box.x + 16, box.y + 14, box.w - 32,
                            FontSize::Body,
                            searchTerm_.empty() ? Palette::TextDim : Palette::Text);
}

void App::drawBrowse()
{
    drawSidebar();

    const bool searching = onSearchScreen();
    if (searching) drawSearch();
    if (searching && keyboardOpen_) { drawKeyboard(); return; }

    // ---- grid ----
    if (itemsLoading_) {
        render_.drawText("Loading...", kSidebarWidth + 40, kTopBarHeight + 40,
                         FontSize::Body, Palette::TextDim);
        return;
    }
    if (items_.empty()) {
        // Search is not an empty library, and saying so was confusing.
        std::string message;
        Color color = Palette::TextDim;
        if (searching) {
            if (!searchTerm_.empty()) {
                message = searchRunning_
                    ? "Searching..."
                    : "Nothing found for \"" + searchTerm_ + "\"";
            } else if (!keyboardOpen_) {
                message = "Press A to type";
            }
        } else if (!errorLine_.empty()) {
            message = errorLine_;
            color  = Palette::Danger;
        } else {
            message = "This library is empty";
        }
        render_.drawText(message, kSidebarWidth + 40,
                         kTopBarHeight + (searching ? 100 : 40),
                         FontSize::Body, color);
        return;
    }

    const GridMetrics g = gridMetrics();

    // Breadcrumb: the sidebar already names the library, so this shows only
    // the path taken inside it.
    if (!navStack_.empty()) {
        const std::string path = breadcrumb();
        if (!path.empty()) {
            render_.drawTextClipped(path, g.originX, g.originY - 38,
                                    logicalW_ - g.originX - 40,
                                    FontSize::Body, Palette::TextDim);
        }
    }

    const int rowStride  = g.tileHeight + g.gapY;
    const int scroll     = (int)(gridScrollPx_ + 0.5f);
    const int bodyTop    = g.originY;
    const int bodyBottom = logicalH_ - kHintBarHeight - 8;

    // Clip to the body so a half-scrolled row cannot paint over the top bar
    // or the hints. SDL scales the clip rect with the logical size for us.
    const SDL_Rect clip = { g.originX - 12, bodyTop,
                            logicalW_ - g.originX + 12, bodyBottom - bodyTop };
    SDL_RenderSetClipRect(sdl_, &clip);

    // Draw only the rows the scroll position can actually reveal.
    const int totalRows = ((int)items_.size() + g.columns - 1) / g.columns;
    const int firstRow  = std::max(0, scroll / rowStride);
    const int lastRow   = std::min(totalRows - 1,
                                   (scroll + (bodyBottom - bodyTop)) / rowStride);
    const int firstIndex = firstRow * g.columns;
    const int lastIndex  = std::min((int)items_.size(), (lastRow + 1) * g.columns);

    for (int i = firstIndex; i < lastIndex; ++i) {
        const int col = i % g.columns;
        const int row = i / g.columns;

        const int x  = g.originX + col * (g.tileWidth + g.gapX);
        const int yy = bodyTop + row * rowStride - scroll;
        const SDL_Rect tile = { x, yy, g.tileWidth, g.tileHeight };

        const bool focused = (i == itemIndex_ && !sidebarFocused_);

        // Episode stills are wide, so a shorter fill height still gives the
        // server enough pixels without shipping an oversized image.
        drawTile(items_[i], tile, focused);

        const Color labelColor = focused ? Palette::Text : Palette::TextDim;
        // Episodes read as "3. Title" with the runtime underneath; everything
        // else is title over year.
        std::string primaryLabel = items_[i].name;
        std::string secondLabel;

        if (curKind_ == LevelKind::Episodes) {
            if (items_[i].indexNumber > 0) {
                char prefix[16];
                std::snprintf(prefix, sizeof(prefix), "%d. ", items_[i].indexNumber);
                primaryLabel = std::string(prefix) + items_[i].name;
            }
            secondLabel = RuntimeText(items_[i].runtimeSeconds());
        } else {
            secondLabel = YearText(items_[i].productionYear);
            // Discovery mixes films and shows in one grid, so each says
            // which it is.
            const char* kind = (activeSource_ == MediaSource::Seerr)
                             ? MediaKindLabel(items_[i]) : "";
            if (kind[0]) {
                secondLabel = secondLabel.empty()
                            ? std::string(kind)
                            : secondLabel + "   " + kind;
            }
        }

        render_.drawTextClipped(primaryLabel, x, yy + g.tileHeight + 10,
                                g.tileWidth, FontSize::Small, labelColor);
        if (!secondLabel.empty()) {
            render_.drawText(secondLabel, x, yy + g.tileHeight + 34,
                             FontSize::Small, Palette::TextDim);
        }
    }

    SDL_RenderSetClipRect(sdl_, nullptr);

    // Scroll bar, drawn only when the library does not fit on one screen.
    const int bodyHeight = bodyBottom - bodyTop;
    const int contentHeight = totalRows * rowStride;
    if (contentHeight > bodyHeight) {
        const int trackX = logicalW_ - 22;
        render_.fillRoundedRect({ trackX, bodyTop, 5, bodyHeight }, 2, Palette::Panel);

        const int thumbH = std::max(40, bodyHeight * bodyHeight / contentHeight);
        const int travel = bodyHeight - thumbH;
        const int maxScroll = contentHeight - bodyHeight;
        const int thumbY = bodyTop + (maxScroll > 0 ? travel * scroll / maxScroll : 0);
        render_.fillRoundedRect({ trackX, thumbY, 5, thumbH }, 2, Palette::Accent);
    }

    if (browseMenuOpen_) drawBrowseMenu();
}

void App::drawDetail()
{
    const int margin = 60;

    // Episodes have 16:9 stills, films have 2:3 posters. Framing a still in
    // a poster box crops it to nothing recognizable.
    const bool episode = (detailItem_.type == "Episode") ||
                         !detailItem_.seriesName.empty();
    const int posterW = episode ? 440 : 300;
    const int posterH = episode ? (posterW * 9) / 16 : 450;

    const SDL_Rect poster = { margin, kTopBarHeight + 40, posterW, posterH };
    render_.fillRoundedRect(poster, 8, Palette::Panel);

    SDL_Texture* tex = art_->get(detailItem_.id, detailItem_.primaryTag,
                                 posterW, posterH, detailItem_.artUrl);
    if (tex) render_.drawTextureCover(tex, poster);

    // How far in, under the poster, where it does not compete with the title.
    if (detailItem_.partiallyWatched()) {
        const SDL_Rect track = { margin, poster.y + posterH + 14, posterW, 6 };
        render_.fillRoundedRect(track, 3, Palette::PanelHi);
        SDL_Rect filled = track;
        filled.w = (int)(posterW * std::min(1.0, detailItem_.playedFraction));
        if (filled.w > 0) render_.fillRoundedRect(filled, 3, Palette::Accent);
    }

    const int textX = margin + posterW + 48;
    const int textW = logicalW_ - textX - margin;
    int y = kTopBarHeight + 40;

    // Episodes lead with the series, since the episode title alone rarely
    // says what you are looking at.
    if (!detailItem_.seriesName.empty()) {
        render_.drawTextClipped(detailItem_.seriesName, textX, y, textW,
                                FontSize::Body, Palette::AccentWarm);
        y += 34;
    }

    y += render_.drawTextWrapped(detailItem_.name, textX, y, textW,
                                 FontSize::Huge, Palette::Text, 2);
    y += 10;

    // One line of facts, joined only where there is something to join.
    std::string meta;
    auto append = [&meta](const std::string& piece) {
        if (piece.empty()) return;
        if (!meta.empty()) meta += "   -   ";
        meta += piece;
    };
    if (detailItem_.parentIndex > 0 && detailItem_.indexNumber > 0) {
        char code[16];
        std::snprintf(code, sizeof(code), "S%02dE%02d",
                      detailItem_.parentIndex, detailItem_.indexNumber);
        append(code);
    }
    append(YearText(detailItem_.productionYear));
    append(RuntimeText(detailItem_.runtimeSeconds()));
    append(detailItem_.officialRating);
    if (detailItem_.communityRating > 0.0) {
        char rating[16];
        const std::string ratingText = bj::ToString(detailItem_.communityRating, 1);
        std::snprintf(rating, sizeof(rating), "%s", ratingText.c_str());
        append(rating);
    }
    if (!meta.empty()) {
        render_.drawText(meta, textX, y, FontSize::Body, Palette::AccentWarm);
        y += 38;
    }

    if (!detailItem_.genres.empty()) {
        render_.drawTextClipped(detailItem_.genres, textX, y, textW,
                                FontSize::Small, Palette::TextDim);
        y += 32;
    }

    if (detailItem_.source == MediaSource::Seerr) {
        const char* status = RequestStatusLabel(detailItem_.requestStatus);
        if (status[0]) {
            render_.drawText(status, textX, y, FontSize::Body,
                             detailItem_.requestStatus == kRequestAvailable
                                 ? Palette::AccentWarm : Palette::Accent);
            y += 38;
        }
    } else if (detailItem_.partiallyWatched()) {
        const int seconds = detailItem_.resumeSeconds();
        char resume[64];
        if (seconds >= 3600) {
            std::snprintf(resume, sizeof(resume), "Resume from %d:%02d:%02d",
                          seconds / 3600, (seconds / 60) % 60, seconds % 60);
        } else {
            std::snprintf(resume, sizeof(resume), "Resume from %d:%02d",
                          seconds / 60, seconds % 60);
        }
        render_.drawText(resume, textX, y, FontSize::Body, Palette::Accent);
        y += 38;
    } else if (detailItem_.played) {
        render_.drawText(detailItem_.source == MediaSource::Seerr
                             ? "Already in your library" : "Watched",
                         textX, y, FontSize::Body, Palette::TextDim);
        y += 38;
    }

    y += 6;
    if (!detailItem_.overview.empty()) {
        y += render_.drawTextWrapped(detailItem_.overview, textX, y, textW,
                                     FontSize::Body, Palette::TextDim, 6);
        y += 16;
    }

    // Directors and the first few names on the bill, which is what a detail
    // screen is usually consulted for once the description has been read.
    std::string directors, cast;
    int castCount = 0;
    for (const JfPerson& person : detailItem_.people) {
        if (person.type == "Director") {
            if (!directors.empty()) directors += ", ";
            directors += person.name;
        } else if (person.type == "Actor" && castCount < 6) {
            if (!cast.empty()) cast += ", ";
            cast += person.name;
            ++castCount;
        }
    }
    auto line = [&](const char* label, const std::string& value) {
        if (value.empty()) return;
        render_.drawText(label, textX, y, FontSize::Small, Palette::TextDim);
        render_.drawTextClipped(value, textX + 100, y, textW - 100,
                                FontSize::Small, Palette::Text);
        y += 28;
    };
    line("Director", directors);
    line("Cast", cast);
    line("Studio", detailItem_.studios);
    if (!detailItem_.chapters.empty()) {
        line("Chapters", bj::ToString((int)detailItem_.chapters.size()));
    }

    if (detailItem_.isFavorite) {
        render_.drawText("Favorite", textX, y, FontSize::Small, Palette::AccentWarm);
    }
}

namespace {

const char* kPlayerMenuTitles[] = { "Audio", "Subtitles", "Version", "Chapters" };

constexpr int kPlayerMenuWidth = 420;

}  // namespace

std::vector<int> App::playerMenuTabs() const
{
    std::vector<int> tabs;
    const JfMediaSource* source = playPlan_.source();
    if (source && source->ofType("Audio").size() > 1) tabs.push_back(0);
    if (source && !source->ofType("Subtitle").empty()) tabs.push_back(1);
    if (playPlan_.sources.size() > 1) tabs.push_back(2);
    if (!playingItem_.chapters.empty()) tabs.push_back(3);
    return tabs;
}

int App::playerMenuOptionCount(int tab) const
{
    const JfMediaSource* source = playPlan_.source();
    if (!source) return 0;
    switch (tab) {
        case 0: return (int)source->ofType("Audio").size();
        case 1: return (int)source->ofType("Subtitle").size() + 1;
        case 2: return (int)playPlan_.sources.size();
        case 3: return (int)playingItem_.chapters.size();
        default: return 0;
    }
}

void App::applyPlayerMenuChoice(int tab, int row)
{
    const JfMediaSource* source = playPlan_.source();
    if (!source) return;

    if (tab == 0) {
        std::vector<const JfStream*> audio = source->ofType("Audio");
        if (row < 0 || row >= (int)audio.size()) return;
        if (audio[(size_t)row]->index == playAudioIndex_) return;
        playAudioIndex_ = audio[(size_t)row]->index;
        restartPlaybackWithTracks();
        return;
    }

    if (tab == 1) {
        if (row == 0) {
            playSubtitleIndex_ = -1;
            subtitleCues_.clear();
            subtitleCueHint_ = 0;
            ++subtitleRequestId_;
            toast("Subtitles off");
            return;
        }
        std::vector<const JfStream*> subs = source->ofType("Subtitle");
        const int which = row - 1;
        if (which < 0 || which >= (int)subs.size()) return;
        const JfStream* stream = subs[(size_t)which];
        if (stream->index == playSubtitleIndex_) return;
        playSubtitleIndex_ = stream->index;
        if (stream->isText) {
            // Fetched and drawn here, so the stream itself is untouched.
            loadSubtitleTrack(stream->index);
        } else {
            // A bitmap track only exists inside the picture, so the server
            // has to draw it and the stream restarts.
            subtitleCues_.clear();
            restartPlaybackWithTracks();
        }
        return;
    }

    if (tab == 3) {
        if (row < 0 || row >= (int)playingItem_.chapters.size()) return;
        seekTargetSeconds_ = (double)playingItem_.chapters[(size_t)row].seconds();
        seekApplyAtMs_ = Platform::NowMs();
        return;
    }

    if (tab == 2) {
        if (row < 0 || row >= (int)playPlan_.sources.size()) return;
        if (row == playSourceIndex_) return;
        playSourceIndex_ = row;
        // A different file has different track numbering.
        playAudioIndex_    = -1;
        playSubtitleIndex_ = -1;
        subtitleCues_.clear();
        restartPlaybackWithTracks();
    }
}

void App::restartPlaybackWithTracks()
{
    if (!player_) return;
    JfItem target = playingItem_;
    const double position =
        player_->startOffsetSeconds() + player_->positionSeconds();
    target.resumeTicks = (int64_t)(position * 10000000.0);
    playerMenuOpen_ = false;
    startPlayback(target, false);
}

void App::loadSubtitleTrack(int streamIndex)
{
    subtitleCues_.clear();
    subtitleCueHint_ = 0;
    if (streamIndex < 0) return;

    const int requestId = ++subtitleRequestId_;
    const std::string itemId = playingItem_.id;
    const std::string sourceId = playPlan_.mediaSourceId;
    subtitlesLoading_ = true;
    pool_.submit([this, requestId, itemId, sourceId, streamIndex] {
        std::string error;
        std::vector<JellyfinClient::JfCue> cues =
            client_.subtitles(itemId, sourceId, streamIndex, error);
        subtitlesLoading_ = false;
        pool_.post([this, requestId, cues, error] {
            if (requestId != subtitleRequestId_) return;
            if (cues.empty()) {
                toast(error.empty() ? "No subtitles in that track" : error);
                return;
            }
            subtitleCues_    = cues;
            subtitleCueHint_ = 0;
        });
    });
}

void App::drawSubtitles(const SDL_Rect& video, int bandTop)
{
    if (subtitleCues_.empty() || !player_) return;

    const double now = player_->startOffsetSeconds() + player_->positionSeconds();

    // Playback runs forward, so the search starts where the last one landed
    // and only walks back when the viewer has seeked.
    if (subtitleCueHint_ >= (int)subtitleCues_.size() ||
        subtitleCues_[(size_t)subtitleCueHint_].start > now) {
        subtitleCueHint_ = 0;
    }
    int found = -1;
    for (int i = subtitleCueHint_; i < (int)subtitleCues_.size(); ++i) {
        const JellyfinClient::JfCue& cue = subtitleCues_[(size_t)i];
        if (cue.start > now) break;
        subtitleCueHint_ = i;
        if (cue.end >= now) { found = i; break; }
    }
    if (found < 0) return;

    // Lines are drawn bottom up so a two line cue grows upward from the
    // same baseline a one line cue sits on.
    std::vector<std::string> lines;
    const std::string& text = subtitleCues_[(size_t)found].text;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        lines.push_back(text.substr(pos, eol - pos));
        pos = eol + 1;
    }

    const int lineHeight = 34;
    int baseline = bandTop > 0
                 ? bandTop + 12
                 : video.y + video.h - 24 - (int)lines.size() * lineHeight;
    if (baseline < 0) baseline = 0;

    const int cx = video.w > 0 ? video.x + video.w / 2 : logicalW_ / 2;
    for (size_t i = 0; i < lines.size(); ++i) {
        const int y = baseline + (int)i * lineHeight;
        // Drawn once in black behind and offset, which reads on any picture
        // without needing a box behind the text.
        render_.drawText(lines[i], cx + 2, y + 2, FontSize::Title,
                         Palette::Outline, Align::Center);
        render_.drawText(lines[i], cx, y, FontSize::Title,
                         Palette::Text, Align::Center);
    }
}

void App::handlePlayerMenuAction(Action action)
{
    std::vector<int> tabs = playerMenuTabs();
    if (tabs.empty()) { playerMenuOpen_ = false; return; }
    if (playerMenuTab_ >= (int)tabs.size()) playerMenuTab_ = 0;

    const int tab = tabs[(size_t)playerMenuTab_];
    const int count = playerMenuOptionCount(tab);

    switch (action) {
        case Action::Back:
        case Action::Options:
            playerMenuOpen_ = false;
            break;
        case Action::Left:
            if (playerMenuTab_ > 0) { --playerMenuTab_; playerMenuRow_ = 0; }
            break;
        case Action::Right:
            if (playerMenuTab_ + 1 < (int)tabs.size()) { ++playerMenuTab_; playerMenuRow_ = 0; }
            break;
        case Action::Up:
            if (playerMenuRow_ > 0) --playerMenuRow_;
            break;
        case Action::Down:
            if (playerMenuRow_ + 1 < count) ++playerMenuRow_;
            break;
        case Action::Accept:
            applyPlayerMenuChoice(tab, playerMenuRow_);
            playerMenuOpen_ = false;
            break;
        default:
            break;
    }
}

void App::drawPlayerMenu()
{
    std::vector<int> tabs = playerMenuTabs();
    if (tabs.empty()) return;
    if (playerMenuTab_ >= (int)tabs.size()) playerMenuTab_ = 0;

    const int tab = tabs[(size_t)playerMenuTab_];
    const JfMediaSource* source = playPlan_.source();
    if (!source) return;

    const int panelW = kPlayerMenuWidth;
    const int panelX = logicalW_ - panelW - 40;
    const int panelY = 60;
    const int panelH = logicalH_ - 160;
    render_.fillRoundedRect({ panelX, panelY, panelW, panelH }, 12, Palette::Panel);

    int x = panelX + 20;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const bool active = (int)i == playerMenuTab_;
        const std::string title = kPlayerMenuTitles[tabs[i]];
        const int width = render_.textWidth(title, FontSize::Body) + 24;
        if (active) {
            render_.fillRoundedRect({ x - 10, panelY + 14, width, 34 }, 8,
                                    Palette::Accent);
        }
        render_.drawText(title, x + 2, panelY + 20, FontSize::Body,
                         active ? Palette::Text : Palette::TextDim);
        x += width + 8;
    }

    const int rowHeight = 40;
    const int listTop = panelY + 66;
    const int visible = (panelH - 76) / rowHeight;
    int first = 0;
    if (playerMenuRow_ >= visible) first = playerMenuRow_ - visible + 1;

    const int count = playerMenuOptionCount(tab);
    for (int i = first; i < count && i - first < visible; ++i) {
        const int y = listTop + (i - first) * rowHeight;
        const bool focused = (i == playerMenuRow_);
        if (focused) {
            render_.fillRoundedRect({ panelX + 10, y - 4, panelW - 20, rowHeight - 4 },
                                    8, Palette::PanelHi);
        }

        std::string label;
        bool current = false;
        if (tab == 0) {
            std::vector<const JfStream*> audio = source->ofType("Audio");
            label   = audio[(size_t)i]->label();
            current = audio[(size_t)i]->index == playAudioIndex_;
        } else if (tab == 1) {
            if (i == 0) {
                label   = "Off";
                current = playSubtitleIndex_ < 0;
            } else {
                std::vector<const JfStream*> subs = source->ofType("Subtitle");
                label   = subs[(size_t)(i - 1)]->label();
                current = subs[(size_t)(i - 1)]->index == playSubtitleIndex_;
            }
        } else if (tab == 2) {
            const JfMediaSource& src = playPlan_.sources[(size_t)i];
            label = src.name.empty() ? src.container : src.name;
            if (label.empty()) label = "Version " + bj::ToString(i + 1);
            current = (i == playSourceIndex_);
        } else {
            const JfChapter& chapter = playingItem_.chapters[(size_t)i];
            const int seconds = chapter.seconds();
            char clock[16];
            std::snprintf(clock, sizeof(clock), "%d:%02d:%02d",
                          seconds / 3600, (seconds / 60) % 60, seconds % 60);
            label = std::string(clock) + "   " +
                    (chapter.name.empty() ? "Chapter " + bj::ToString(i + 1)
                                          : chapter.name);
        }

        render_.drawTextClipped(label, panelX + 24, y, panelW - 70,
                                FontSize::Body,
                                focused ? Palette::Text : Palette::TextDim);
        // A dot rather than a glyph: the console fonts are ASCII only.
        if (current) {
            render_.fillRoundedRect({ panelX + panelW - 34, y + 9, 10, 10 }, 5,
                                    Palette::Accent);
        }
    }
}

void App::drawPlayer()
{
    render_.clear({ 0, 0, 0 });

    const Player::State state = player_ ? player_->state() : Player::State::Idle;
    const int cx = logicalW_ / 2;

    if (state == Player::State::Failed) {
        render_.drawText("Cannot play this", cx, logicalH_ / 2 - 30,
                         FontSize::Title, Palette::Text, Align::Center);
        render_.drawText(player_ ? player_->errorText() : "", cx, logicalH_ / 2 + 20,
                         FontSize::Body, Palette::Danger, Align::Center);
        return;
    }

    const bool showControls = player_ && (player_->paused() ||
                                          seekTargetSeconds_ >= 0.0 ||
                                          Platform::NowMs() < controlsUntilMs_);

    // Controls take a strip at the bottom and the picture shrinks above them
    // rather than being drawn over. The Wii U draws video with the GPU
    // directly, which leaves shaders and textures bound that SDL believes are
    // still its own, so nothing may be drawn after it.
    const int controlsHeight = showControls ? 140 : 0;

    // The Wii U draws the picture last and with the GPU directly, so nothing
    // can be laid over it. Subtitles get a band of their own there instead.
#if defined(__WIIU__)
    const int subtitleBand = subtitleCues_.empty() ? 0 : 80;
#else
    const int subtitleBand = 0;
#endif
    const int videoBottom = logicalH_ - controlsHeight - subtitleBand;

    if (showControls && player_) {
        const int top = videoBottom;
        render_.fillRect({ 0, top, logicalW_, controlsHeight }, Palette::Panel);

        render_.drawTextClipped(playingItem_.name, 40, top + 14,
                                logicalW_ - 320, FontSize::Title, Palette::Text);

        // While a seek is pending, show where it will land rather than where
        // playback still is: the number the viewer is aiming at.
        const double position = (seekTargetSeconds_ >= 0.0)
            ? seekTargetSeconds_
            : player_->startOffsetSeconds() + player_->positionSeconds();
        const double duration = playingItem_.runtimeSeconds() > 0
            ? (double)playingItem_.runtimeSeconds()
            : player_->durationSeconds();

        auto timeText = [](double seconds) {
            if (seconds < 0) seconds = 0;
            const int total = (int)seconds;
            char buf[32];
            if (total >= 3600) {
                std::snprintf(buf, sizeof(buf), "%d:%02d:%02d",
                              total / 3600, (total / 60) % 60, total % 60);
            } else {
                std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
            }
            return std::string(buf);
        };

        const std::string clock = timeText(position) +
                                  (duration > 0 ? "  /  " + timeText(duration) : "");
        render_.drawText(clock, logicalW_ - 40, top + 22, FontSize::Body,
                         Palette::TextDim, Align::Right);

        const SDL_Rect track = { 40, top + 66, logicalW_ - 80, 6 };
        render_.fillRoundedRect(track, 3, Palette::PanelHi);
        if (duration > 0.0) {
            const double fraction = std::min(1.0, std::max(0.0, position / duration));
            SDL_Rect filled = track;
            filled.w = (int)(track.w * fraction + 0.5);
            if (filled.w > 0) {
                render_.fillRoundedRect(filled, 3,
                    seekTargetSeconds_ >= 0.0 ? Palette::AccentWarm : Palette::Accent);
            }
        }

        const char* status = (seekTargetSeconds_ >= 0.0) ? "Seeking"
                           : player_->paused()           ? "Paused" : "";
        if (status[0]) {
            render_.drawText(status, 40, top + 84, FontSize::Small, Palette::Accent);
        }

        if (showDebug_ && settings_.diagnostics) {
            char stats[128];
            std::snprintf(stats, sizeof(stats), "queue %d  segments %d  dropped %d",
                          player_->queuedFrames(), player_->bufferedSegments(),
                          player_->droppedFrames());
            render_.drawText(stats, logicalW_ - 40, top + 84, FontSize::Small,
                             Palette::AccentWarm, Align::Right);
        }
    }

    if (state == Player::State::Opening || state == Player::State::Buffering) {
        const int phase = (int)((Platform::NowMs() / 350) % 4);
        render_.drawText(state == Player::State::Opening ? "Asking the server"
                                                         : "Buffering",
                         cx, videoBottom / 2 - 20, FontSize::Title,
                         Palette::Text, Align::Center);
        render_.drawText(std::string(phase, '.'), cx, videoBottom / 2 + 30,
                         FontSize::Huge, Palette::Accent, Align::Center);
        return;
    }

    // Letterbox inside whatever height is left. Sources are rarely exactly
    // 16:9 once the studio's framing is accounted for.
    const int frameW = haveVideoFrame_ ? currentVideoFrame_.width  : videoWidth_;
    const int frameH = haveVideoFrame_ ? currentVideoFrame_.height : videoHeight_;

    // The Wii U paints the picture last and with the GPU, so anything drawn
    // over that rectangle is lost. The track menu gets a strip of its own
    // there instead of sitting on top.
#if defined(__WIIU__)
    const int menuInset = playerMenuOpen_ ? kPlayerMenuWidth + 60 : 0;
#else
    const int menuInset = 0;
#endif
    const int videoWidth = logicalW_ - menuInset;

    SDL_Rect target{ 0, 0, 0, 0 };
    if (frameW > 0 && frameH > 0) {
        // The server's aspect wins where it gave one, since a frame's own
        // dimensions say nothing about non-square pixels.
        const double reported = player_ ? player_->displayAspect() : 0.0;
        const double videoAspect = reported > 0.0
            ? reported : (double)frameW / (double)frameH;
        const double areaAspect  = (double)videoWidth / (double)videoBottom;
        if (videoAspect > areaAspect) {
            target.w = videoWidth;
            target.h = (int)(videoWidth / videoAspect + 0.5);
        } else {
            target.h = videoBottom;
            target.w = (int)(videoBottom * videoAspect + 0.5);
        }
        target.x = (videoWidth - target.w) / 2;
        target.y = (videoBottom - target.h) / 2;
    }

#ifdef __WIIU__
    if (gx2Ready_ && haveVideoFrame_ && target.w > 0) {
        drawSubtitles(target, videoBottom);
        if (playerMenuOpen_) drawPlayerMenu();
        // SDL queues its drawing; flush it so the interface is already on
        // the frame before the GPU draws the picture over what remains.
        SDL_RenderFlush(sdl_);

        const float left   = (float)target.x / logicalW_ * 2.0f - 1.0f;
        const float right  = (float)(target.x + target.w) / logicalW_ * 2.0f - 1.0f;
        const float top    = 1.0f - (float)target.y / logicalH_ * 2.0f;
        const float bottom = 1.0f - (float)(target.y + target.h) / logicalH_ * 2.0f;
        gx2Video_.draw(currentVideoFrame_, left, top, right, bottom);
    } else
#endif
    if (videoTexture_ && videoWidth_ > 0 && videoHeight_ > 0) {
        SDL_RenderCopy(sdl_, videoTexture_, nullptr, &target);
    }

#if !defined(__WIIU__)
    drawSubtitles(target, 0);
    if (playerMenuOpen_) drawPlayerMenu();
#endif
}

void App::drawToast()
{
    if (toastText_.empty() || Platform::NowMs() > toastUntilMs_) return;

    const int w = render_.textWidth(toastText_, FontSize::Body) + 56;
    const int h = 56;
    const SDL_Rect box = { (logicalW_ - w) / 2, logicalH_ - kHintBarHeight - h - 24,
                           w, h };
    render_.fillRoundedRect(box, 10, Palette::PanelHi);
    render_.drawText(toastText_, logicalW_ / 2, box.y + 15,
                     FontSize::Body, Palette::Text, Align::Center);
}

void App::drawDebugOverlay()
{
    char line[256];
    std::snprintf(line, sizeof(line),
                  "art:%lu tex  %lu fetching   jobs:%lu   items:%lu   %dx%d",
                  (unsigned long)art_->textureCount(),
                  (unsigned long)art_->inFlightCount(),
                  (unsigned long)pool_.pendingJobs(),
                  (unsigned long)items_.size(), logicalW_, logicalH_);
    render_.fillRect({ 0, kTopBarHeight, logicalW_, 28 }, Palette::Shadow);
    render_.drawText(line, 12, kTopBarHeight + 4, FontSize::Small, Palette::AccentWarm);
}
