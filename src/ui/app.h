// SPDX-License-Identifier: GPL-2.0-or-later

// app.h: screens, focus, and the navigation model.
//
// Everything the user sees lives here. Network calls are dispatched to the
// worker pool and their results posted back to the main thread, so the UI
// keeps drawing at full rate while a library loads.

#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <vector>

#include <SDL.h>

#ifdef __WIIU__
#include "platform/wiiu/gx2_video.h"
#endif

#include "core/jellyfin.h"
#include "core/settings.h"
#include "core/player.h"
#include "core/video_frame.h"
#include "core/worker.h"
#include "ui/art_cache.h"

#include <map>
#include "ui/render.h"

// A d-pad-shaped action, whatever produced it. Keyboard, GamePad, Pro
// Controller and Classic Controller all land here.
enum class Action {
    None, Up, Down, Left, Right,
    Accept, Back, Menu, Refresh,
    PageUp, PageDown,
    Quit,
};

// How deep into a library the user has navigated. Movies stop at Library;
// shows go Library -> Seasons -> Episodes.
enum class LevelKind { Library, Seasons, Episodes };

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
    ServerSelect,   // discovered servers
    SignIn,         // Quick Connect code on screen, polling
    Home,           // rows: carry on watching, next up, recently added
    Browse,         // sidebar of libraries + poster grid
    Settings,       // server, account, quality, about
    About,          // credits and third party notices
    Detail,         // one item
    Playing,        // video on screen
};

// One horizontal row on the home screen. Rows are how a television library
// is usually presented, and they suit a d-pad better than a grid does when
// the sections are short.
struct HomeRow {
    std::string title;
    std::vector<JfItem> items;
    int  focus = 0;      // which item is selected
    int  scroll = 0;     // first visible item
};

// An entry in the sidebar. Home and Search sit above the libraries.
struct SidebarEntry {
    enum class Kind { Home, Search, Library, Settings } kind = Kind::Library;
    std::string title;
    std::string libraryId;
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

    // Replays a fixed list of actions, one every `intervalMs`, before the
    // auto-screenshot fires. Lets a screen several levels deep be verified
    // without a person holding the controller.
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
    void openSidebarEntry(int index);
    void loadHome();
    void handleHomeAction(Action action);
    // True when the sidebar's Search entry is the one selected.
    bool onSearchScreen() const;
    void beginSearch();
    // An on-screen keyboard of our own. The console's system keyboard is
    // raised through SDL_StartTextInput and takes the app down with it, and
    // a keyboard drawn here works with any controller besides.
    void drawKeyboard();
    void handleKeyboardAction(Action action);
    void runSearch(const std::string& term);
    void openDetail(int itemIndex);
    void startPlayback(const JfItem& item, bool fromStart);
    void stopPlayback();
    void handlePlayerAction(Action action);
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
    void drawSearch();
    void drawSidebar();
    void drawSettings();
    void drawAbout();
    void handleSettingsAction(Action action);
    void switchServer();
    void drawTile(const JfItem& item, const SDL_Rect& tile, bool focused);
    void drawDetail();
    void drawPlayer();
    void drawToast();
    void drawDebugOverlay();

    void toast(const std::string& message);

    // Writes the current frame to DataDir()/screenshots. On the console this
    // is the only practical way to see what the app actually drew, since the
    // build machine is not attached to the TV.
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
    WorkerPool               pool_{ 4 };
    std::unique_ptr<ArtCache> art_;

    Screen screen_ = Screen::Connecting;
    bool   running_ = true;

    // Connecting / status
    std::string statusLine_ = "Starting up";
    std::string errorLine_;

    // Server selection
    std::vector<JfServer> servers_;
    int  serverIndex_ = 0;
    std::atomic<bool> discovering_{ false };

    // Sign-in
    std::string quickConnectCode_;
    std::string signInServerName_;
    uint64_t    nextPollMs_ = 0;
    uint64_t    signInDeadlineMs_ = 0;
    std::atomic<bool> pollInFlight_{ false };

    // Browse
    std::vector<JfLibrary>    libraries_;
    std::vector<SidebarEntry> sidebar_;
    int  libraryIndex_ = 0;      // index into sidebar_, not libraries_
    bool sidebarFocused_ = true;

    // Home
    std::vector<HomeRow> homeRows_;
    int  homeRow_ = 0;
    std::atomic<bool> homeLoading_{ false };

    // Settings
    Settings settings_;
    int      settingsRow_ = 0;
    // Signing out is asked twice, and only from Settings. It was on Start
    // everywhere, which is a button people press by accident.
    bool     confirmSignOut_ = false;
    uint64_t confirmUntilMs_ = 0;   // set from kPlaybackMaxHeight at startup

    // Search
    std::string searchTerm_;
    bool keyboardOpen_ = false;
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
    // Bumped on every library switch; a late reply with a stale id is dropped
    // so fast d-pad scrolling cannot show the wrong library's contents.
    int  itemsRequestId_ = 0;

    // Library listings, kept for the session: moving along the sidebar
    // otherwise refetches every item each time.
    std::map<std::string, std::vector<JfItem>> itemsCache_;

    // Detail
    JfItem detailItem_;

    // Playback
    std::unique_ptr<Player> player_;

    // One converter thread per core the streaming and decoding threads are
    // not already using. _XENON is tested first: _XBOX is defined on both
    // Xboxes.
#if defined(__WIIU__)
    Nv12Converter videoConverter_{ 2 };
#elif defined(_XBOX) && !defined(_XENON)
    Nv12Converter videoConverter_{ 1 };
#else
    Nv12Converter videoConverter_{ 3 };
#endif
    // Three textures in rotation. Locking a texture the GPU is still reading
    // blocks until it finishes, which measured as a flat ~17ms per frame no
    // matter how large the frame was: a pipeline fence, not a copy. Writing
    // to a different texture each frame means the lock never waits.
    static constexpr int kVideoTextureCount = 3;
    SDL_Texture*  videoTextures_[kVideoTextureCount] = { nullptr, nullptr, nullptr };
    int           videoTextureIndex_ = 0;
    SDL_Texture*  videoTexture_ = nullptr;   // the one holding the newest frame
    // Conversion goes here first, then one bulk copy into the texture.
    // Writing pixel by pixel straight into mapped texture memory is far
    // slower than writing to ordinary RAM: the boot benchmark converts a
    // frame in 13.9ms into a plain buffer and the same code took 19-22ms
    // writing into the locked texture.
    std::vector<uint8_t> videoStaging_;

    // Channel byte offsets for the texture format actually in use.
    PixelBytes videoBytes_;

    // The frame currently on screen. Held rather than drawn immediately
    // because the GPU path samples it during drawing, not before.
    Nv12Frame     currentVideoFrame_;
    bool          haveVideoFrame_ = false;
#ifdef __WIIU__
    // Draws video straight from the decoder's planes, skipping the colour
    // conversion and SDL's texture upload entirely.
    Gx2Video      gx2Video_;
    bool          gx2Ready_ = false;
#endif
    int           videoWidth_   = 0;
    int           videoHeight_  = 0;
    JfItem        playingItem_;
    uint64_t      controlsUntilMs_ = 0;   // on-screen controls auto-hide
    uint64_t      lastProgressReportMs_ = 0;
    double        seekTargetSeconds_ = -1.0;
    bool          endHandled_ = false;
    uint64_t      seekApplyAtMs_ = 0;

    // Rolling playback cost, reported to the log every couple of seconds.
    // Guessing at where the time goes on hardware nobody can attach a
    // profiler to has not worked well.
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

    // Every controller the port maps has the same layout: GamePad, Pro
    // Controller, Classic Controller, Wii Remote and Nunchuk. They are held
    // open so any of them can be picked up at any time, and opened as they
    // appear rather than only at startup.
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
