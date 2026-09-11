// SPDX-License-Identifier: GPL-2.0-or-later

// app.h: screens, focus, and the navigation model.
//
// Network calls go to the worker pool and their results are posted back, so
// the UI keeps drawing while a library loads.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <vector>

#include <SDL.h>

#ifdef __WIIU__
#include "platform/wiiu/gx2_video.h"
#endif

#include "core/jellyfin.h"
#include "core/seerr.h"
#include "core/features.h"
#include "core/settings.h"
#include "core/player.h"
#include "core/video_frame.h"
#include "core/worker.h"
#include "ui/art_cache.h"
#include "ui/input_hint.h"

#include <map>
#include "ui/render.h"

// A d-pad-shaped action, whatever produced it. Keyboard, GamePad, Pro
// Controller and Classic Controller all land here.
enum class Action {
    None, Up, Down, Left, Right,
    Accept, Back, Menu, Refresh,
    // The X button, whose meaning is the screen's.
    Options,
    PageUp, PageDown,
    Quit,
};

// How deep into a library the user has navigated. Movies stop at Library;
// shows go Library -> Seasons -> Episodes.
enum class LevelKind { Library, Seasons, Episodes, Folder };

// A browse level the user can come back to. Pushed on the way in so that
// going back restores the exact focus and scroll position they left.
struct BrowseLevel {
    LevelKind   kind = LevelKind::Library;
    std::string title;
    std::string parentId;
    std::string seriesId;
    std::vector<JfItem> items;
    int   itemIndex      = 0;
    float scrollPx       = 0.0f;
    float scrollTargetPx = 0.0f;
};

enum class Screen {
    Connecting,     // bringing up the network, restoring a session
    Welcome,        // first run, nothing configured yet
    ServerSelect,   // discovered servers
    SignIn,         // Quick Connect code on screen, polling
    Home,           // rows: carry on watching, next up, recently added
    Browse,         // sidebar of libraries + poster grid
    Settings,       // server, account, quality, about
    About,          // credits and third party notices
    Detail,         // one item
    Playing,        // video on screen
};

// One horizontal row on the home screen.
struct HomeRow {
    std::string title;
    std::vector<JfItem> items;
    int  focus = 0;      // which item is selected
    int  scroll = 0;     // first visible item
};

// An entry in the sidebar. Home and Search sit above the libraries.
struct SidebarEntry {
    // Services is the tab row at the bottom, moved through left and right.
    // Discover is one of Seerr's lists; Requests is what was asked for.
    enum class Kind { Home, Search, Discover, Requests, Library, Settings,
                      Services } kind = Kind::Library;
    std::string title;
    std::string libraryId;
    int         mode = 0;      // Discover: which list. Requests: which filter.
};

class App {
public:
    App();
    ~App();

    bool init(SDL_Window* window, SDL_Renderer* renderer);

    // Headless verification: draw for `delayMs`, write one PNG to `path`,
    // then quit. Lets a build be checked without a person watching a screen.
    void setAutoScreenshot(const std::string& path, int delayMs);
    void setSettings(const Settings& settings);

    // Replays actions, one every `intervalMs`, before the screenshot fires,
    // so a screen several levels deep can be reached unattended.
    void setScriptedInput(const std::vector<Action>& actions, int intervalMs);
    void shutdown();

    // Returns false once the user has asked to quit.
    bool frame();

private:
    // ---- input ----
    Action translate(const SDL_Event& event) const;
    // Directions from a held d-pad or stick, repeated while held. Menus are
    // long enough that stepping once per press is not usable.
    void   pumpHeldDirection();
    void   openController(int deviceIndex);
    void   closeController(SDL_JoystickID which);
    void   handleAction(Action action);
    void   handleBrowseAction(Action action);
    void   drawBrowseMenu();
    void   handleBrowseMenuAction(Action action);
    int    browseMenuOptionCount(int tab) const;
    void   applyBrowseMenuChoice(int tab, int row);
    // What the grid query should carry, given the menu's current state.
    void   applyBrowseFilters(JfQuery& query) const;
    // Changes with the view, so two views cannot share a cached listing.
    std::string browseCacheKey(const std::string& libraryId) const;
    void   loadBrowseFilterOptions(const std::string& libraryId);
    // Fetches the next page as the selection nears the end.
    void   maybeLoadMore();
    // Drops any paging state, so a new listing does not append to the old.
    void   resetPaging();
    // Clears the grid and points it at a new listing, then takes the first
    // page when it lands. Shared by every screen that is a grid of items.
    int    resetGrid(LevelKind kind, const std::string& title);
    int    beginGridLoad(const std::string& title);
    void   firstPage(int requestId, const std::vector<JfItem>& items,
                     const std::string& error, int nextPage, int total);
    void   appendPage(int requestId, const std::vector<JfItem>& items,
                      int nextPage, int total);
    void   handleDetailAction(Action action);
    void   handleServerSelectAction(Action action);

    // ---- flow ----
    void startup();                  // session restore or discovery
    void beginDiscovery();
    void beginSignIn(const std::string& serverUrl);
    void pollQuickConnect();
    void loadLibraries();
    void loadLibraryItems(int libraryIndex);
    void rebuildSidebar();
    void switchSource(MediaSource source);
    void drawBottomBar();
    void drawServerRow(int y);
    void drawWelcome();
    void handleWelcomeAction(Action action);
    std::string activeServerName() const;
    void beginManualServer();
    void commitManualServer();
    // Setting up the request server: address, then the same Quick Connect
    // the Jellyfin side uses, so there is no key to type.
    std::string requestServerSummary() const;
    // Tries Seerr beside the library, as the same account. Silent.
    // Test leaves the live client alone; Save takes the form on.
    void testRequestServer(bool keep);
    // Pushes the playback choices down to the client that builds
    // the stream URL.
    void applyStreamPrefs();
    void tryRequestServerBesideJellyfin();
    void beginRequestServer();
    void commitRequestServer();
    void pollRequestQuickConnect();
    void loadBarIcons();
    void drawIcon(SDL_Texture* icon, const SDL_Rect& box, Color tint,
                  bool recolor);
    bool handleSidebarNav(Action action, bool canLeave);
    bool handleServiceTabs(Action action);
    void updateLayoutSize();
    // Keeps the window size in settings so it comes back where it was left.
    void rememberWindowSize();
    void loadDiscover();
    void loadRequests(int filter);
    void openSidebarEntry(int index);
    void loadHome();
    void handleHomeAction(Action action);
    // True when the sidebar's Search entry is the one selected.
    bool onSearchScreen() const;
    void beginSearch();
    // Ours, because SDL_StartTextInput raises the console's own and takes
    // the app down with it.
    void drawKeyboard();
    // A pad gets the drawn keyboard; a real one does not.
    bool onScreenKeyboardWanted() const;
    void handleKeyboardAction(Action action);
    void runSearch(const std::string& term);
    void openDetail(int itemIndex);
    void startPlayback(const JfItem& item, bool fromStart);
    void stopPlayback();
    void handlePlayerAction(Action action);
    // Audio, subtitle and version selection, over the picture.
    void drawPlayerMenu();
    void handlePlayerMenuAction(Action action);
    // Which of Audio, Subtitles and Version this item actually offers.
    std::vector<int> playerMenuTabs() const;
    int  playerMenuOptionCount(int tab) const;
    void applyPlayerMenuChoice(int tab, int row);
    // Restarts the stream at the current position with different tracks.
    void restartPlaybackWithTracks();
    void loadSubtitleTrack(int streamIndex);
    void drawSubtitles(const SDL_Rect& video, int bandTop);
    // Seeking restarts the stream at an offset, so repeated presses are
    // gathered up and applied once the viewer stops pressing.
    void seekBy(int deltaSeconds);
    void applyPendingSeek();
    // Called once when a film or episode reaches its end.
    void handlePlaybackEnded();
    // Pulls the frame that is due, converts it, and uploads it to the video
    // texture. Returns false when nothing new was ready.
    bool updateVideoTexture();
    void logPlaybackStats();

    // Set when the video textures are made: true if the renderer takes YUV
    // planes and converts on the GPU, false if we convert to RGB ourselves.
    bool videoUploadsYuv_ = false;

    // Drill in and out of a series.
    void enterItem(int itemIndex);
    void loadSeasons(const std::string& seriesId, const std::string& title);
    // A collection, an album, or anything else that is a bag of items.
    void loadFolder(const std::string& folderId, const std::string& title);
    void loadEpisodes(const std::string& seriesId, const std::string& seasonId,
                      const std::string& title);
    void pushCurrentLevel();
    bool popLevel();                  // false when already at the top
    std::string breadcrumb() const;
    void signOut();

    // ---- drawing ----
    void draw();
    void drawChrome();               // top bar and bottom hints
    void drawConnecting();
    void drawServerSelect();
    void drawSignIn();
    void drawBrowse();
    void drawHome();
    // Tiles across a home row, from the width actually available.
    int  homeColumns() const;
    // Pixel height of one row, tile plus its title and labels.
    int  homeRowHeight(size_t row) const;
    void clampHomeScroll();
    void drawSearch();
    void drawSidebar();
    void drawSettings();
    // Built each frame: choosing manual Seerr opens more rows under it.
    enum class SettingAction {
        Heading,                 // a divider, not something to land on
        JellyfinServer, JellyfinAccount,
        RequestAlongside, RequestElsewhere,
        RequestAddress, RequestUser, RequestPassword, RequestTest, RequestSave,
        Display, Playback, VideoBitrate, Framerate,
        AudioFormat, AudioBitrate,
        Diagnostics, DiagnosticsNote, About,
    };
    struct SettingsEntry {
        SettingAction action;
        std::string   label;
        std::string   value;
        std::string   hint;
        bool          indented = false;
        // -1 when the row is not one of a choice.
        int           radio = -1;
    };
    std::vector<SettingsEntry> settingsRows() const;
    // Headings are drawn but not landed on, so moving steps over them.
    int  nextSettingsRow(const std::vector<SettingsEntry>& rows,
                         int from, int step) const;
    void drawAbout();
    void handleSettingsAction(Action action);
    void switchServer();
    void useInputScheme(InputScheme scheme);
    void drawHints(const std::string& text, int x, int y);
    void drawTile(const JfItem& item, const SDL_Rect& tile, bool focused);
    void drawDetail();
    void drawPlayer();
    void drawToast();
    void drawDebugOverlay();

    void toast(const std::string& message);

    // Writes the current frame to DataDir()/screenshots, which is the only
    // way to see what a console actually drew.
    void saveScreenshot();
    bool writeScreenshot(const std::string& path);

    // Grid geometry, derived once per frame from the logical size.
    struct GridMetrics {
        int columns;
        int tileWidth, tileHeight;
        int gapX, gapY;
        int originX, originY;
        int visibleRows;
    };
    GridMetrics gridMetrics() const;
    void        clampGridScroll();     // retargets the scroll for itemIndex_
    void        updateScroll();        // eases toward the target, once a frame

    // ---- state ----
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* sdl_      = nullptr;
    Renderer      render_;

    JellyfinClient           client_;
    SeerrClient              seerr_;

    // Which service the sidebar and grid are showing. Seerr only appears
    // once seerr.txt names one.
    MediaSource              activeSource_ = MediaSource::Jellyfin;
    // The fixed bar: tabs 0..N-1 are services, N is the gear.
    SDL_Texture*             iconJellyfin_ = nullptr;
    SDL_Texture*             iconSeerr_    = nullptr;
    SDL_Texture*             iconGear_     = nullptr;

    bool                     bottomBarFocused_ = false;
    int                      bottomTab_ = 0;
    int                      bottomRow_ = 1;   // 0 server, 1 services
    int                      welcomeRow_ = 0;
    int                      sidebarScroll_ = 0;
    int                      settingsScroll_ = 0;
    // The home screen is showing Seerr's rows rather than the library's.
    bool                     showingSeerr_ = false;
    WorkerPool               pool_{ 4 };
    std::unique_ptr<ArtCache> art_;

    Screen screen_ = Screen::Connecting;
    bool   running_ = true;

    // Connecting / status
    std::string statusLine_ = "Starting up";
    std::string errorLine_;

    // Server selection
    std::vector<JfServer>  servers_;
    int  serverIndex_ = 0;
    std::atomic<bool> discovering_{ false };

    // Sign-in
    std::string quickConnectCode_;
    // Quick Connect against the request server, which runs alongside the
    // Jellyfin one rather than replacing it.
    std::string seerrCode_;
    uint64_t    seerrPollAtMs_ = 0;
    uint64_t    seerrDeadlineMs_ = 0;
    std::atomic<bool> seerrPollInFlight_{ false };
    // So the silent attempt is made once per run, not once per visit.
    bool seerrAutoTried_ = false;
    std::string seerrPairedWith_;
    // What the form under "my own request server" has collected so far.
    std::string seerrFormAddress_;
    std::string seerrFormUser_;
    std::string seerrFormPassword_;
    std::string signInServerName_;
    uint64_t    nextPollMs_ = 0;
    uint64_t    signInDeadlineMs_ = 0;
    std::atomic<bool> pollInFlight_{ false };

    // Browse
    std::vector<JfLibrary>    libraries_;
    std::vector<SidebarEntry> sidebar_;
    int  libraryIndex_ = 0;      // index into sidebar_, not libraries_
    bool sidebarFocused_ = true;

    // How the grid is ordered and what it leaves out. Kept across libraries,
    // so a viewer who wants unwatched first only says so once.
    bool browseMenuOpen_   = false;
    int  browseMenuTab_    = 0;
    int  browseMenuRow_    = 0;
    int  browseSort_       = 0;    // index into JfSortOptions
    bool browseDescending_ = false;
    int  browseShow_       = 0;    // All, Unwatched, Watched, Favorites, Started
    std::string browseGenre_;      // empty means every genre

    // Discovery is one grid, not a stack of rows: which list it shows and
    // which genre it is narrowed to live here.
    int         discoverMode_ = 0;
    std::string discoverGenre_;      // the query fragment, empty for any
    std::string discoverGenreName_;
    std::vector<JfItem> discoverGenres_;
    std::string discoverGenresFor_;   // "movie" or "tv", whichever they are
    JfFilterOptions browseFilters_;
    std::string browseFiltersFor_; // the library browseFilters_ describes

    // Home
    std::vector<HomeRow> homeRows_;
    int   homeRow_ = 0;
    float homeScrollPx_       = 0.0f;
    float homeScrollTargetPx_ = 0.0f;
    std::atomic<bool> homeLoading_{ false };

    // Settings
    Settings settings_;
    int      settingsRow_ = 0;
    // Signing out is asked twice, and only from Settings.
    bool     confirmSignOut_ = false;
    uint64_t confirmUntilMs_ = 0;   // set from kPlaybackMaxHeight at startup

    // Search
    std::string searchTerm_;
    // What the on-screen keyboard is collecting.
    enum class KeyboardFor { Search, ServerAddress,
                            RequestServerAddress, RequestUser, RequestPassword };
    // Shared by both ways in: opens text entry for whatever purpose says.
    void beginTextEntry(KeyboardFor purpose);
    void commitTextEntry();
    void cancelTextEntry();
    KeyboardFor keyboardFor_ = KeyboardFor::Search;
    bool keyboardOpen_ = false;
    // Real keystrokes instead of the drawn keyboard.
    bool typingDirect_ = false;
    int  keyRow_ = 0;
    int  keyCol_ = 0;
    std::atomic<bool> searchRunning_{ false };

    // The level currently on screen.
    std::vector<JfItem> items_;
    LevelKind   curKind_ = LevelKind::Library;
    std::string curTitle_;
    std::string curParentId_;
    std::string curSeriesId_;
    std::vector<BrowseLevel> navStack_;   // levels to return to

    int   itemIndex_ = 0;
    // Scroll is tracked in pixels and eased toward its target, so the grid
    // glides instead of snapping a whole row at a time.
    float gridScrollPx_       = 0.0f;
    float gridScrollTargetPx_ = 0.0f;
    std::atomic<bool> itemsLoading_{ false };

    // Paging. pageNext_ is zero once the server has nothing more.
    std::function<void(int)> pageLoader_;
    int  pageNext_  = 0;
    int  pageTotal_ = 0;    // what the server says it holds, 0 when unknown
    std::atomic<bool> pageLoading_{ false };
    // Bumped on every library switch; a late reply with a stale id is dropped
    // so fast d-pad scrolling cannot show the wrong library's contents.
    int  itemsRequestId_ = 0;
    std::string fontDir_;

#if defined(__WIIU__)
    InputScheme inputScheme_ = InputScheme::Nintendo;
#elif defined(_XBOX)
    InputScheme inputScheme_ = InputScheme::Xbox;
#else
    InputScheme inputScheme_ = InputScheme::Keyboard;
#endif

    // Kept for the session, or moving along the sidebar refetches every
    // time. The first page only, and where the rest continues from.
    struct CachedListing {
        std::vector<JfItem> items;
        int nextPage = 0;
        int total    = 0;
    };
    std::map<std::string, CachedListing> itemsCache_;

    // Detail
    JfItem detailItem_;

    // Playback
    std::unique_ptr<Player> player_;

    // One thread per core the streaming and decoding threads are not using.
#if defined(__WIIU__)
    Nv12Converter videoConverter_{ 2 };
#elif defined(_XBOX) && !defined(_XENON)
    Nv12Converter videoConverter_{ 1 };
#else
    Nv12Converter videoConverter_{ 3 };
#endif
    // In rotation: locking one the GPU is still reading blocks for a flat
    // 17ms whatever its size, so each frame writes to a different one.
    static constexpr int kVideoTextureCount = 3;
    SDL_Texture*  videoTextures_[kVideoTextureCount] = { nullptr, nullptr, nullptr };
    int           videoTextureIndex_ = 0;
    SDL_Texture*  videoTexture_ = nullptr;   // the one holding the newest frame
    // Converted here, then copied in one go. Writing straight into mapped
    // texture memory costs 19ms a frame against 14ms into ordinary RAM.
    std::vector<uint8_t> videoStaging_;

    // Channel byte offsets for the texture format actually in use.
    PixelBytes videoBytes_;

    // The frame currently on screen. Held rather than drawn immediately
    // because the GPU path samples it during drawing, not before.
    Nv12Frame     currentVideoFrame_;
    bool          haveVideoFrame_ = false;
#ifdef __WIIU__
    // Draws video straight from the decoder's planes, skipping the color
    // conversion and SDL's texture upload entirely.
    Gx2Video      gx2Video_;
    bool          gx2Ready_ = false;
#endif
    int           videoWidth_   = 0;
    int           videoHeight_  = 0;
    JfItem        playingItem_;

    // Track selection. -1 means whatever the server chose, and for subtitles
    // it also means none once the menu has been opened.
    JellyfinClient::PlaybackPlan playPlan_;
    int  playSourceIndex_   = 0;
    int  playAudioIndex_    = -1;
    int  playSubtitleIndex_ = -1;
    bool playerMenuOpen_    = false;
    int  playerMenuTab_     = 0;
    int  playerMenuRow_     = 0;
    std::vector<JellyfinClient::JfCue> subtitleCues_;
    int  subtitleCueHint_   = 0;   // where the last lookup landed
    std::atomic<bool> subtitlesLoading_{ false };
    int  subtitleRequestId_ = 0;
    uint64_t      controlsUntilMs_ = 0;   // on-screen controls auto-hide
    uint64_t      lastProgressReportMs_ = 0;
    double        seekTargetSeconds_ = -1.0;
    bool          endHandled_ = false;
    uint64_t      seekApplyAtMs_ = 0;

    // Rolling playback cost, for hardware no profiler can attach to.
    int      statFrames_    = 0;
    double   statConvertMs_ = 0.0;
    double   statUploadMs_  = 0.0;
    uint64_t statLastLogMs_ = 0;
    double   statPresentMs_ = 0.0;
    // Per main-loop-iteration, as opposed to per video frame: how long the UI
    // took to draw and how long the present blocked waiting for a vblank.
    int      statLoops_       = 0;
    double   statDrawMs_      = 0.0;
    double   statPresentSumMs_ = 0.0;

    // Toast
    std::string toastText_;
    uint64_t    toastUntilMs_ = 0;

    // False while the console has taken the foreground away (HOME menu). The
    // app must stop drawing until it gets the foreground back.
    bool foreground_ = true;

    // Held open as they appear, so any of them can be picked up at any time.
    std::vector<SDL_GameController*> controllers_;

    Action   heldDirection_ = Action::None;
    uint64_t heldNextRepeatMs_ = 0;
    // Which sticks currently read as a direction, kept apart from the d-pad
    // so releasing one does not cancel the other.
    Action   stickDirection_ = Action::None;
    Action   dpadDirection_  = Action::None;

    bool showDebug_ = false;
    int  screenshotCounter_ = 0;

    std::string autoShotPath_;
    uint64_t    autoShotAtMs_ = 0;

    std::vector<Action> scriptedActions_;
    size_t              scriptIndex_   = 0;
    int                 scriptIntervalMs_ = 1200;
    uint64_t            nextScriptedMs_ = 0;
    int  logicalW_ = 1280;
    int  logicalH_ = 720;
};
