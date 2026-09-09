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

// How a decoded frame reaches the screen. _XENON is tested first: _XBOX is
// defined on both Xboxes and they take different paths here.
#if defined(_XENON)
constexpr Uint32     kVideoYuvFormat  = SDL_PIXELFORMAT_IYUV;
constexpr bool       kPreferYuvUpload = true;
constexpr Uint32     kVideoRgbFormat  = SDL_PIXELFORMAT_ARGB8888;
constexpr PixelOrder kVideoPixelOrder = PixelOrder::Argb;
#elif defined(_XBOX)
// YUY2, converted by the NV2A while sampling. Packing costs a byte shuffle
// instead of a colour conversion and halves the upload. See PackNv12ToYuy2.
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

// Whether the renderer really takes YUV. SDL accepts a format it does not
// list and converts every frame on the CPU instead, which is slower than
// doing it here.
bool RendererTakesYuv(SDL_Renderer* renderer)
{
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) != 0) return false;
    for (Uint32 i = 0; i < info.num_texture_formats; ++i) {
        if (info.texture_formats[i] == kVideoYuvFormat) return true;
    }
    return false;
}

// Playback height requested from the server. The Wii U converts colour on the
// CPU and cannot hold 720p in the frame budget; the 360 converts on the GPU
// and can. _XENON is tested first: _XBOX is defined on both Xboxes.
#if defined(__WIIU__)
constexpr int kPlaybackMaxHeight = 480;
#elif defined(_XENON)
constexpr int kPlaybackMaxHeight = 720;
#elif defined(_XBOX)
constexpr int kPlaybackMaxHeight = 480;
#else
constexpr int kPlaybackMaxHeight = 720;
#endif

// Quick Connect codes are short-lived; give up rather than poll forever.
constexpr uint64_t kSignInWindowMs = 5 * 60 * 1000;
constexpr uint64_t kPollIntervalMs = 2000;

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

// Servers listed in DataDir()/server.txt, one base URL per line. Discovery
// depends on broadcast traffic surviving the network between console and
// server, which is not something the app can guarantee; this is the way to
// name a server explicitly without needing an on-screen keyboard.
std::vector<JfServer> ReadServerOverrides()
{
    std::vector<JfServer> out;

    // Beside the settings first, then beside the executable. The second is
    // there for consoles whose writable directory is awkward to reach from a
    // development host: on the Xbox 360 the data directory is the title's own
    // cache device, which the debug bridge cannot write to, while the launch
    // directory is an ordinary folder on the hard disk.
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

// What to show in a library when the user opens it. Music libraries open on
// albums rather than on every individual track.
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

    const std::string fontDir = Platform::AssetDir() + "/fonts";
    if (!render_.init(renderer, fontDir)) {
        errorLine_ = "Could not load fonts from " + fontDir;
        LOGF("[ui] %s", errorLine_.c_str());
        return false;
    }
    LOGF("[ui] fonts loaded from %s", fontDir.c_str());

    art_ = std::unique_ptr<ArtCache>(new ArtCache(renderer, client_, pool_));

#ifdef __WIIU__
    // If this fails the player falls back to the CPU path, which works but
    // costs about 25ms a frame.
    // A shader dropped on the SD card wins over the bundled one, so a
    // diagnostic shader can be tried without rebuilding and reinstalling.
    std::string gx2Error;
    const std::string overrideShader = Platform::DataDir() + "/shader.gsh";
    const std::string shaderPath = Platform::FileExists(overrideShader)
                                 ? overrideShader
                                 : Platform::AssetDir() + "/shaders/nv12.gsh";
    LOGF("[gx2] shader: %s", shaderPath.c_str());
    gx2Ready_ = gx2Video_.init(shaderPath, gx2Error);
    if (!gx2Ready_) LOGF("[gx2] unavailable, using the CPU path: %s", gx2Error.c_str());
#endif

    // Without a logical size set, which is the case on the Xbox 360, this
    // reports zero and the renderer's own output size is
    // what the interface is being drawn into.
    SDL_RenderGetLogicalSize(renderer, &logicalW_, &logicalH_);
    if (logicalW_ <= 0 || logicalH_ <= 0) {
        SDL_GetRendererOutputSize(renderer, &logicalW_, &logicalH_);
    }
    if (logicalW_ <= 0 || logicalH_ <= 0) {
        logicalW_ = 1280;
        logicalH_ = 720;
    }
    LOGF("[ui] drawing at %dx%d", logicalW_, logicalH_);

    startup();
    return true;
}

void App::shutdown()
{
    stopPlayback();

    // Cut any transfer still running first. Without this, shutdown waits for
    // a slow download to finish, and the console does not give an app that
    // long to quit after the user presses HOME.
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
    // SDL_RenderReadPixels copies the GX2 colour buffer as if it were linear,
    // but it is tiled, so the result is repeated and interlaced rubbish.
    // Aroma's screenshot plugin untiles properly, so defer to it rather than
    // writing a file that looks broken.
    toast("Use the screenshot plugin (writes to sd:/wiiu/screenshots)");
    LOGF("[shot] declined: GX2 readback is tiled, use Aroma's plugin");
    return;
#else
    const std::string dir = Platform::DataDir() + "/screenshots";
    Platform::MakeDirs(dir);

    // Monotonic counter rather than a timestamp: the console's clock is not
    // guaranteed to be set, and duplicate names would silently overwrite.
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
            } else {
                beginDiscovery();
            }
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
    screen_           = Screen::SignIn;
    statusLine_       = "Contacting the server";
    errorLine_.clear();
    quickConnectCode_.clear();
    client_.setServerUrl(serverUrl);

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
    sidebar_.push_back({ SidebarEntry::Kind::Home,   "Home",   "" });
    sidebar_.push_back({ SidebarEntry::Kind::Search, "Search", "" });
    for (const JfLibrary& library : libraries_) {
        sidebar_.push_back({ SidebarEntry::Kind::Library, library.name, library.id });
    }
    sidebar_.push_back({ SidebarEntry::Kind::Settings, "Settings", "" });
}

void App::openSidebarEntry(int index)
{
    if (index < 0 || index >= (int)sidebar_.size()) return;
    libraryIndex_ = index;

    switch (sidebar_[index].kind) {
        case SidebarEntry::Kind::Home:
            screen_ = Screen::Home;
            if (homeRows_.empty() && !homeLoading_) loadHome();
            break;
        case SidebarEntry::Kind::Search:
            // Selecting only shows the search screen. Text entry starts on
            // A, so moving the selection past Search does not capture the
            // keyboard on the way to something else.
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
            settingsRow_ = 0;
            break;
    }
}

// Three rows, fetched together. Each is short, so one pass costs little and
// the home screen appears complete rather than filling in piecemeal.
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

    pool_.submit([this] {
        std::string error;
        std::vector<JfLibrary> libs = client_.libraries(error);
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
            // Open on Home rather than the first library: it is where a
            // half-watched episode is, which is what someone sitting down
            // usually wants.
            openSidebarEntry(0);
        });
    });
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

    std::map<std::string, std::vector<JfItem>>::const_iterator hit =
        itemsCache_.find(library.id);
    if (hit != itemsCache_.end()) {
        items_        = hit->second;
        itemsLoading_ = false;
        errorLine_.clear();
        return;
    }

    pool_.submit([this, library, requestId] {
        JfQuery q;
        q.parentId         = library.id;
        q.includeItemTypes = DefaultItemTypesFor(library.collectionType);
        q.sortBy           = "SortName";
        q.limit            = 500;

        std::string error;
        std::vector<JfItem> fetched = client_.items(q, error);

        LOGF("[library] %s: %lu item(s)%s%s", library.name.c_str(),
             (unsigned long)fetched.size(),
             error.empty() ? "" : " - ", error.c_str());
        const std::string libraryId = library.id;
        pool_.post([this, fetched, error, requestId, libraryId] {
            // The user moved on before this landed; drop it.
            if (requestId != itemsRequestId_) return;

            items_        = fetched;
            itemIndex_    = 0;
            gridScrollPx_ = 0.0f;
            gridScrollTargetPx_ = 0.0f;
            itemsLoading_ = false;
            if (fetched.empty() && !error.empty()) {
                errorLine_ = error;
            } else {
                errorLine_.clear();
                if (!fetched.empty()) itemsCache_[libraryId] = fetched;
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
    openDetail(itemIndex);
}

void App::loadSeasons(const std::string& seriesId, const std::string& title)
{
    curKind_     = LevelKind::Seasons;
    curTitle_    = title;
    curParentId_ = seriesId;
    curSeriesId_ = seriesId;
    items_.clear();
    itemIndex_   = 0;
    gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
    itemsLoading_ = true;
    errorLine_.clear();

    const int requestId = ++itemsRequestId_;
    pool_.submit([this, seriesId, requestId] {
        std::string error;
        std::vector<JfItem> fetched = client_.seasons(seriesId, error);
        pool_.post([this, fetched, error, requestId] {
            if (requestId != itemsRequestId_) return;
            items_        = fetched;
            itemIndex_    = 0;
            itemsLoading_ = false;
            if (fetched.empty()) {
                errorLine_ = error.empty() ? "No seasons here" : error;
            }
        });
    });
}

void App::loadEpisodes(const std::string& seriesId, const std::string& seasonId,
                       const std::string& title)
{
    curKind_     = LevelKind::Episodes;
    curTitle_    = title;
    curParentId_ = seasonId;
    curSeriesId_ = seriesId;
    items_.clear();
    itemIndex_   = 0;
    gridScrollPx_ = gridScrollTargetPx_ = 0.0f;
    itemsLoading_ = true;
    errorLine_.clear();

    const int requestId = ++itemsRequestId_;
    pool_.submit([this, seriesId, seasonId, requestId] {
        std::string error;
        std::vector<JfItem> fetched = client_.episodes(seriesId, seasonId, error);
        pool_.post([this, fetched, error, requestId] {
            if (requestId != itemsRequestId_) return;
            items_        = fetched;
            itemIndex_    = 0;
            itemsLoading_ = false;
            if (fetched.empty()) {
                errorLine_ = error.empty() ? "No episodes here" : error;
            }
        });
    });
}

void App::openDetail(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= (int)items_.size()) return;
    detailItem_ = items_[itemIndex];
    screen_     = Screen::Detail;

    // The grid query omits Overview to keep the listing small, so fetch the
    // full record now that we are showing one item.
    const std::string itemId = detailItem_.id;
    pool_.submit([this, itemId] {
        JfItem full;
        std::string error;
        if (!client_.itemDetails(itemId, full, error)) return;
        pool_.post([this, full, itemId] {
            if (screen_ == Screen::Detail && detailItem_.id == itemId) {
                detailItem_ = full;
            }
        });
    });
}

void App::startPlayback(const JfItem& item, bool fromStart)
{
    stopPlayback();

    playingItem_ = item;
    player_.reset(new Player());

    // Pick up where the server says this was left, unless asked not to.
    const double startSeconds = fromStart ? 0.0 : (double)item.resumeSeconds();
    if (startSeconds > 0.0) {
        LOGF("[player] resuming at %d seconds", (int)startSeconds);
    }

    std::string error;
    const int height = settings_.playbackHeight < kPlaybackMaxHeight
                     ? settings_.playbackHeight : kPlaybackMaxHeight;
    if (!player_->open(client_, item.id, height, startSeconds, error)) {
        toast(error.empty() ? "Could not start playback" : error);
        player_.reset();
        return;
    }

    screen_ = Screen::Playing;
    endHandled_ = false;
    controlsUntilMs_ = Platform::NowMs() + 3000;
    statFrames_ = 0;
    statConvertMs_ = statUploadMs_ = 0.0;
    // The loop counters run the whole time the app is up, so without this the
    // first window after playback starts reports every menu frame as well and
    // its averages mean nothing.
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

    // Absolute position in the film, not the offset within this stream: a
    // resumed stream starts partway in, and the server needs where the
    // viewer actually got to.
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

// Gathers repeated presses rather than restarting the stream on each one.
// A seek means asking the server for a stream that begins somewhere else, so
// pressing right five times should cost one request, not five.
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

// Reaching the end of an episode should lead into the next one. Watching a
// series otherwise means going back to the menu between every episode.
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

    switch (action) {
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
        if (now - statLastLogMs_ >= 2000 && statFrames_ > 0) {
            LOGF("[player] %d frames in %lums: copy %s flush %s draw %s "
                 "present %s, queue %d, segments %d, decoded %d, dropped %d",
                 statFrames_, (unsigned long)(now - statLastLogMs_),
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

    // Rebuild when the stream's size changes, which happens once at the
    // start and never again for a given item.
    if (frame.width != videoWidth_ || frame.height != videoHeight_ ||
        !videoTextures_[0]) {
        for (SDL_Texture*& texture : videoTextures_) {
            if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        }
        videoUploadsYuv_ = kPreferYuvUpload && RendererTakesYuv(sdl_);
        const Uint32 format = videoUploadsYuv_ ? kVideoYuvFormat : kVideoRgbFormat;

        for (SDL_Texture*& texture : videoTextures_) {
            // A format the renderer advertises. Ask for one it does not and
            // SDL keeps a shadow texture and converts every frame on unlock,
            // which measured 88ms a frame against 3ms for a plain copy.
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
        // Straight from the decoder to the GPU. SDL takes the three planes,
        // puts each in its own texture and lets its YUV pixel shader do the
        // colour conversion, so the CPU's whole contribution to getting this
        // frame on screen is the copy below: 1.5 bytes a pixel, against 4
        // written by an RGB conversion that also had to compute them.
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

    // Convert is the colour conversion alone; upload is what lock and unlock
    // cost around it, which on this renderer includes tiling the result.
    statConvertMs_ += (double)(convertEnd - convertStart);
    statUploadMs_  += (double)(unlockEnd - convertEnd);
    ++statFrames_;

    logPlaybackStats();
    return true;
}

// Averages of what the last couple of seconds cost per frame. Both are what
// the main thread spent: convert is the colour conversion, upload is what
// getting the result into a texture cost around it.
void App::logPlaybackStats()
{
    if (!player_ || statFrames_ <= 0) return;

    const uint64_t now = Platform::NowMs();
    if (statLastLogMs_ == 0) statLastLogMs_ = now;
    if (now - statLastLogMs_ < 2000) return;

    // Two rates, because they answer different questions. Frames is how much
    // video reached the screen; loops is how fast the main thread went round,
    // and with vsync on a 60Hz output a loop that overruns 16.7ms costs a
    // whole vblank. When frames is well under the source rate while the queue
    // stays full, it is this second number that explains it.
    const int loops = statLoops_ > 0 ? statLoops_ : 1;
    LOGF("[player] %d frames in %lums: convert %s ms, upload %s ms, "
         "queue %d, segments %d, decoded %d, dropped %d",
         statFrames_, (unsigned long)(now - statLastLogMs_),
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
            // The port's mapping carries SDL_GAMECONTROLLER_USE_BUTTON_LABELS,
            // so BUTTON_A really is the button labelled A rather than the one
            // in the Xbox A position. Nintendo's layout needs no swap here.
            case SDL_CONTROLLER_BUTTON_A:          return Action::Accept;
            case SDL_CONTROLLER_BUTTON_B:          return Action::Back;
            case SDL_CONTROLLER_BUTTON_Y:          return Action::Refresh;
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

// Emits a direction while one is held: once immediately, then repeating
// after a pause. Without this, moving through a few hundred films means a
// few hundred presses.
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
            if (serverIndex_ + 1 < (int)servers_.size()) ++serverIndex_;
            break;
        case Action::Refresh:
            beginDiscovery();
            break;
        case Action::Accept:
            if (!servers_.empty()) beginSignIn(servers_[serverIndex_].address);
            else beginDiscovery();
            break;
        default: break;
    }
}

// Settings rows, in the order they are drawn.
namespace {
enum SettingsRow {
    kSettingServer = 0,
    kSettingAccount,
    kSettingDisplay,
    kSettingQuality,
    kSettingAbout,
    kSettingCount,
};

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
    // Signing out first: a token from one server means nothing to another,
    // and leaving it behind would make the next sign-in look already done.
    pool_.submit([this] {
        client_.signOut();
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

void App::handleSettingsAction(Action action)
{
    if (sidebarFocused_) {
        switch (action) {
            case Action::Up:
                if (libraryIndex_ > 0) openSidebarEntry(libraryIndex_ - 1);
                break;
            case Action::Down:
                if (libraryIndex_ + 1 < (int)sidebar_.size())
                    openSidebarEntry(libraryIndex_ + 1);
                break;
            case Action::Right:
            case Action::Accept:
                sidebarFocused_ = false;
                break;
            default: break;
        }
        return;
    }

    switch (action) {
        case Action::Up:
            if (settingsRow_ > 0) --settingsRow_;
            break;
        case Action::Down:
            if (settingsRow_ + 1 < kSettingCount) ++settingsRow_;
            break;
        case Action::Left:
        case Action::Back:
            sidebarFocused_ = true;
            break;
        case Action::Accept:
            switch (settingsRow_) {
                case kSettingServer:  switchServer(); break;
                case kSettingAccount:
                    // Asked twice. Signing out throws away the session and
                    // means going through Quick Connect again, which is a
                    // lot to lose to one stray button.
                    if (confirmSignOut_ && Platform::NowMs() < confirmUntilMs_) {
                        confirmSignOut_ = false;
                        signOut();
                    } else {
                        confirmSignOut_ = true;
                        confirmUntilMs_ = Platform::NowMs() + 5000;
                        toast("Press A again to sign out");
                    }
                    break;
                case kSettingDisplay:
                    // Which screens get drawn to is fixed when the window is
                    // created, so this takes effect next launch rather than
                    // tearing the renderer down mid-session.
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
                case kSettingQuality:
                    // Cycles through the heights worth offering. 720 lets the
                    // server copy the stream; the lower two make it re-encode,
                    // which is worth having for a slow network.
                    settings_.playbackHeight =
                        (settings_.playbackHeight == 720) ? 480
                      : (settings_.playbackHeight == 480) ? 360 : 720;
                    if (settings_.playbackHeight > kPlaybackMaxHeight) {
                        settings_.playbackHeight = kPlaybackMaxHeight;
                    }
                    settings_.save();
                    toast("Playback quality: " +
                          bj::ToString(settings_.playbackHeight) + "p");
                    break;
                case kSettingAbout:   screen_ = Screen::About; break;
                default: break;
            }
            break;
        default: break;
    }
}

void App::handleHomeAction(Action action)
{
    if (action == Action::Refresh) { loadHome(); return; }

    if (sidebarFocused_) {
        switch (action) {
            case Action::Up:
                if (libraryIndex_ > 0) openSidebarEntry(libraryIndex_ - 1);
                break;
            case Action::Down:
                if (libraryIndex_ + 1 < (int)sidebar_.size())
                    openSidebarEntry(libraryIndex_ + 1);
                break;
            case Action::Right:
            case Action::Accept:
                if (!homeRows_.empty()) sidebarFocused_ = false;
                break;
            default: break;
        }
        return;
    }

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
        case Action::Back:
            sidebarFocused_ = true;
            break;
        case Action::Accept:
            if (row.focus < (int)row.items.size()) {
                const JfItem& item = row.items[row.focus];
                // A series here means the next episode is what is wanted, so
                // only a folder needs drilling into.
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

// The keyboard's layout. The last row holds the wider keys, which are
// handled by name rather than by character.
const char* const kKeyRows[] = {
    "1234567890",
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz-'&:",
};
constexpr int kKeyRowCount = 4;
constexpr int kKeyColCount = 10;

// Row 4: space, backspace, done.
constexpr int kActionRow = kKeyRowCount;

}  // namespace

void App::beginSearch()
{
    curKind_  = LevelKind::Library;
    curTitle_ = "Search";
    navStack_.clear();
    sidebarFocused_ = false;
    keyboardOpen_   = true;
    keyRow_ = 1;
    keyCol_ = 0;
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
                // The bottom row has three wide keys, not ten narrow ones.
                // Without clamping, coming down from column five landed on a
                // key that does not exist and nothing highlighted at all.
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
                    case 2: keyboardOpen_ = false; break;
                }
            } else if (keyCol_ < kKeyColCount) {
                searchTerm_ += kKeyRows[keyRow_][keyCol_];
            }
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
    const int keyW = 62, keyH = 54, gap = 8;
    const int gridW = kKeyColCount * keyW + (kKeyColCount - 1) * gap;
    const int originX = kSidebarWidth + (logicalW_ - kSidebarWidth - gridW) / 2;
    const int originY = kTopBarHeight + 108;

    // A panel behind it, so results showing through do not make the letters
    // hard to read.
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
    const struct { const char* label; int width; } actions[] = {
        { "Space", 240 }, { "Delete", 180 }, { "Done", 180 },
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
        std::vector<JfItem> found = client_.search(term, error);
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
    if (action == Action::Refresh) {
        switch (curKind_) {
            case LevelKind::Library:  openSidebarEntry(libraryIndex_); break;
            case LevelKind::Seasons:  loadSeasons(curSeriesId_, curTitle_); break;
            case LevelKind::Episodes: loadEpisodes(curSeriesId_, curParentId_,
                                                   curTitle_); break;
        }
        return;
    }

    if (sidebarFocused_) {
        switch (action) {
            case Action::Up:
                if (libraryIndex_ > 0) openSidebarEntry(libraryIndex_ - 1);
                break;
            case Action::Down:
                if (libraryIndex_ + 1 < (int)sidebar_.size())
                    openSidebarEntry(libraryIndex_ + 1);
                break;
            case Action::Right:
                if (!items_.empty()) sidebarFocused_ = false;
                break;
            case Action::Accept:
                if (libraryIndex_ < (int)sidebar_.size() &&
                    sidebar_[libraryIndex_].kind == SidebarEntry::Kind::Search) {
                    beginSearch();
                } else if (!items_.empty()) {
                    sidebarFocused_ = false;
                }
                break;
            default: break;
        }
        return;
    }

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
}

void App::handleDetailAction(Action action)
{
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
        // Foreground handover. Cafe OS expects an app to stop drawing
        // promptly when the user opens the HOME menu; carrying on issuing
        // GPU work after the display has been taken away is what hangs the
        // console rather than backgrounding it.
        // SDL sends both WILL* and DID* for each transition, so act only on
        // an actual change of state. Otherwise every handover threw the
        // texture caches away twice, including once during startup.
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
            showDebug_ = !showDebug_;
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
    // this renderer synchronises with the GPU. None of it is needed while a
    // film is on screen.
    if (screen_ != Screen::Playing) {
        art_->processCompleted(2);
        updateScroll();
    }

    if (screen_ == Screen::Playing) {
        applyPendingSeek();
        updateVideoTexture();

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
    if (showDebug_) drawDebugOverlay();
}

void App::drawChrome()
{
    // Playback owns the whole screen; its own controls replace this chrome.
    if (screen_ == Screen::Playing) return;

    // Top bar
    render_.fillRect({ 0, 0, logicalW_, kTopBarHeight }, Palette::Panel);
    render_.fillRect({ 0, kTopBarHeight - 2, logicalW_, 2 }, Palette::PanelHi);

    // The console's name first, in its own colour, then the project's.
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
        case Screen::Connecting:   hints = errorLine_.empty() ? "" : "A  Retry"; break;
        case Screen::ServerSelect: hints = "A  Connect      Y  Search again"; break;
        case Screen::SignIn:       hints = "B  Back"; break;
        case Screen::Browse:
            if (sidebarFocused_) {
                hints = onSearchScreen() ? "A  Type a search" : "A  Open library";
            } else if (keyboardOpen_) {
                hints = "A  Type      Y  Delete      B  Close keyboard";
            } else if (onSearchScreen() && items_.empty()) {
                hints = "A  Type a search      B  Menu";
            } else {
                const bool drillable = !items_.empty() &&
                    itemIndex_ < (int)items_.size() &&
                    (items_[itemIndex_].type == "Series" ||
                     items_[itemIndex_].type == "Season");
                hints  = drillable ? "A  Open" : "A  Details";
                hints += navStack_.empty() ? "      B  Libraries" : "      B  Back";
                hints += "      Y  Refresh";
            }
            break;
        case Screen::Home:
            hints = sidebarFocused_ ? "A  Open"
                                    : "A  Details      B  Menu      Y  Refresh";
            break;
        case Screen::Settings:
            hints = sidebarFocused_ ? "A  Open"
                                    : "A  Select      B  Menu";
            break;
        case Screen::About:
            hints = "B  Back";
            break;
        case Screen::Detail:
            hints = detailItem_.partiallyWatched()
                ? "A  Resume      Y  From the start      Start  Mark watched      B  Back"
                : "A  Play      Start  Mark watched      B  Back";
            break;
        case Screen::Playing:
            hints = "A  Pause      Left/Right  10s      L/R  1m      Up/Down  5m      B  Stop";
            break;
    }
    if (!hints.empty()) {
        render_.drawText(hints, 40, hintY + 16, FontSize::Small, Palette::TextDim);
    }

    // Position indicator, so a long library does not feel bottomless. It
    // lives on the hint bar rather than over the grid, where it used to sit
    // on top of the first row of posters.
    if (screen_ == Screen::Browse && !items_.empty() && !sidebarFocused_) {
        char counter[64];
        std::snprintf(counter, sizeof(counter), "%d of %lu",
                      itemIndex_ + 1, (unsigned long)items_.size());
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

    int y = kTopBarHeight + 24;

    for (size_t i = 0; i < sidebar_.size(); ++i) {
        // A rule between the fixed entries and the libraries, which are a
        // different kind of thing.
        if (sidebar_[i].kind == SidebarEntry::Kind::Library &&
            (i == 0 || sidebar_[i - 1].kind != SidebarEntry::Kind::Library)) {
            render_.drawText("LIBRARIES", 32, y + 6, FontSize::Small, Palette::TextDim);
            y += 38;
        }

        const bool selected = ((int)i == libraryIndex_);
        const SDL_Rect row = { 20, y, kSidebarWidth - 40, 54 };

        if (selected) {
            render_.fillRoundedRect(row, 8,
                sidebarFocused_ ? Palette::Accent : Palette::PanelHi);
        }
        const Color label = selected ? Palette::Text : Palette::TextDim;
        render_.drawTextClipped(sidebar_[i].title, 38, y + 13,
                                kSidebarWidth - 76, FontSize::Body, label);
        y += 62;
    }
}

// Draws a poster with its label, and a progress bar when the item has been
// started. Shared by the grid and the home rows.
void App::drawTile(const JfItem& item, const SDL_Rect& tile, bool focused)
{
    render_.fillRoundedRect(tile, 6, Palette::Panel);

    // Ask for exactly the box it will be drawn into, so the server does the
    // cropping and scaling and this ends up a 1:1 blit.
    SDL_Texture* poster = art_->get(item.id, item.primaryTag, tile.w, tile.h);
    if (poster) {
        render_.drawTextureCover(poster, tile);
    } else {
        render_.drawTextClipped(item.name, tile.x + 10, tile.y + tile.h / 2 - 12,
                                tile.w - 20, FontSize::Small, Palette::TextDim);
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

    if (focused) render_.strokeRect(tile, Palette::Accent, 4);
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
    // Four across rather than five: at 1280 wide, five 16:9 tiles come out
    // about 165 pixels across, which is too small to recognise from a sofa.
    const int columns   = 4;
    const int gap       = 20;
    const int tileWidth = (available - gap * (columns - 1)) / columns;
    // Home rows mix films and episodes, so the tiles are 16:9 rather than
    // poster shaped: an episode still in a poster frame looks wrong.
    const int tileHeight = (tileWidth * 9) / 16;
    const int rowStride  = tileHeight + 68;

    int y = kTopBarHeight + 16;
    for (size_t r = 0; r < homeRows_.size(); ++r) {
        if (y + tileHeight > logicalH_ - kHintBarHeight) break;

        const HomeRow& row = homeRows_[r];
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

            const JfItem& item = row.items[index];
            const std::string label = item.seriesName.empty()
                ? item.name
                : item.seriesName + " - " + item.name;
            render_.drawTextClipped(label, tile.x, tile.y + tile.h + 8, tileWidth,
                                    FontSize::Small,
                                    focused ? Palette::Text : Palette::TextDim);
        }
        y += rowStride;
    }
}

void App::drawSettings()
{
    drawSidebar();

    const int left  = kSidebarWidth + 48;
    const int width = logicalW_ - left - 48;
    int y = kTopBarHeight + 28;

    render_.drawText("Settings", left, y, FontSize::Title, Palette::Text);
    y += 62;

    struct Row { const char* label; std::string value; const char* action; };
    std::string serverName = client_.serverUrl();
    if (serverName.rfind("http://", 0) == 0) serverName = serverName.substr(7);

    const Row rows[kSettingCount] = {
        { "Server",   serverName.empty() ? "not set" : serverName, "Switch server" },
        { "Account",  client_.userName().empty() ? "not signed in" : client_.userName(),
                      (confirmSignOut_ && Platform::NowMs() < confirmUntilMs_)
                          ? "Press A again" : "Sign out" },
        { "Display",  DisplayLabel(settings_.display), "Change" },
        { "Quality",  bj::ToString(settings_.playbackHeight) + "p maximum",
                      "Change" },
        { "About",    "Version " + std::string(kAppVersion), "Credits" },
    };

    for (int i = 0; i < kSettingCount; ++i) {
        const bool focused = (!sidebarFocused_ && i == settingsRow_);
        const SDL_Rect box = { left, y, width, 74 };
        render_.fillRoundedRect(box, 8, focused ? Palette::PanelHi : Palette::Panel);
        if (focused) render_.strokeRect(box, Palette::Accent, 3);

        render_.drawText(rows[i].label, left + 24, y + 12, FontSize::Body,
                         focused ? Palette::Text : Palette::TextDim);
        render_.drawTextClipped(rows[i].value, left + 24, y + 40, width - 220,
                                FontSize::Small, Palette::TextDim);
        render_.drawText(rows[i].action, left + width - 24, y + 24,
                         FontSize::Small,
                         focused ? Palette::Accent : Palette::TextDim,
                         Align::Right);
        y += 86;
    }
}

void App::drawAbout()
{
    render_.clear(Palette::Background);

    const int cx = logicalW_ / 2;
    int y = 90;

    render_.drawText(kAppName, cx, y, FontSize::Huge, Palette::Text, Align::Center);
    y += 64;
    render_.drawText(std::string("Version ") + kAppVersion +
                         "  -  a Jellyfin client for the " + kPlatformLongName,
                     cx, y, FontSize::Body, Palette::TextDim, Align::Center);
    y += 56;

    // Credits, and nothing else. How the video pipeline works belongs in the
    // source, not on a screen someone opened to see who to thank.
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
    };

    for (const Line& line : lines) {
        if (line.text[0]) {
            render_.drawText(line.text, cx, y, FontSize::Small,
                             line.dim ? Palette::TextDim : Palette::Text,
                             Align::Center);
        }
        y += 30;
    }
}

void App::drawSearch()
{
    const int left = kSidebarWidth + 40;
    const SDL_Rect box = { left, kTopBarHeight + 16, logicalW_ - left - 40, 56 };
    render_.fillRoundedRect(box, 8, Palette::Panel);
    render_.strokeRect(box, Palette::Accent, 2);

    const std::string shown = !searchTerm_.empty() ? searchTerm_
                            : keyboardOpen_ ? "Pick letters with A"
                                            : "Press A to search";
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
        Color colour = Palette::TextDim;
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
            colour  = Palette::Danger;
        } else {
            message = "This library is empty";
        }
        render_.drawText(message, kSidebarWidth + 40,
                         kTopBarHeight + (searching ? 100 : 40),
                         FontSize::Body, colour);
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

}

void App::drawDetail()
{
    const int margin = 60;

    // Episodes have 16:9 stills, films have 2:3 posters. Framing a still in
    // a poster box crops it to nothing recognisable.
    const bool episode = (detailItem_.type == "Episode") ||
                         !detailItem_.seriesName.empty();
    const int posterW = episode ? 440 : 300;
    const int posterH = episode ? (posterW * 9) / 16 : 450;

    const SDL_Rect poster = { margin, kTopBarHeight + 40, posterW, posterH };
    render_.fillRoundedRect(poster, 8, Palette::Panel);

    SDL_Texture* tex = art_->get(detailItem_.id, detailItem_.primaryTag,
                                 posterW, posterH);
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

    // Where playback will pick up, spelled out rather than implied by a bar.
    if (detailItem_.partiallyWatched()) {
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
        render_.drawText("Watched", textX, y, FontSize::Body, Palette::TextDim);
        y += 38;
    }

    y += 6;
    if (!detailItem_.overview.empty()) {
        render_.drawTextWrapped(detailItem_.overview, textX, y, textW,
                                FontSize::Body, Palette::TextDim, 6);
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
    const int videoBottom = logicalH_ - controlsHeight;

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

        if (showDebug_) {
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

    SDL_Rect target{ 0, 0, 0, 0 };
    if (frameW > 0 && frameH > 0) {
        // The server's aspect wins where it gave one, since a frame's own
        // dimensions say nothing about non-square pixels.
        const double reported = player_ ? player_->displayAspect() : 0.0;
        const double videoAspect = reported > 0.0
            ? reported : (double)frameW / (double)frameH;
        const double areaAspect  = (double)logicalW_ / (double)videoBottom;
        if (videoAspect > areaAspect) {
            target.w = logicalW_;
            target.h = (int)(logicalW_ / videoAspect + 0.5);
        } else {
            target.h = videoBottom;
            target.w = (int)(videoBottom * videoAspect + 0.5);
        }
        target.x = (logicalW_ - target.w) / 2;
        target.y = (videoBottom - target.h) / 2;
    }

#ifdef __WIIU__
    if (gx2Ready_ && haveVideoFrame_ && target.w > 0) {
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
