// SPDX-License-Identifier: GPL-2.0-or-later

// jellyfin.h: Jellyfin server client. Discovery, Quick Connect auth, browse,
// and stream URL construction.
//
// Every call here blocks. The UI runs them on a worker thread; the CLI test
// harness calls them straight. Keeping the core synchronous means the same
// code path is exercised both ways.

#pragma once

#include <cstdint>
#include <string>

#include <vector>

// Where an item came from, and so what can be done with it. Jellyfin items
// play; Seerr items are things the library does not have yet, so they are
// asked for instead.
enum class MediaSource { Jellyfin, Seerr };

// A server found by UDP broadcast on the local network.
struct JfServer {
    std::string id;
    std::string name;
    std::string address;   // full base URL, e.g. "http://192.168.1.10:8096"
};

// A top-level library ("Movies", "Music"). collectionType is the machine
// readable kind: "movies", "tvshows", "music", "photos", ...
struct JfLibrary {
    std::string id;
    std::string name;
    std::string collectionType;
};

// One track inside a media source: video, audio or subtitle.
struct JfStream {
    int         index = 0;
    std::string type;          // "Video" | "Audio" | "Subtitle"
    std::string codec;
    std::string language;      // ISO 639-2, "eng"
    std::string title;         // what the server would show in a menu
    std::string deliveryUrl;   // set when the server will hand it over as a file
    int         channels = 0;
    bool        isDefault = false;
    bool        isForced = false;
    bool        isExternal = false;
    bool        isText = false;      // false means a bitmap format, PGS or VOBSUB
    bool        isHearingImpaired = false;

    // "English (SDH)", built from language and codec when there is no title.
    std::string label() const;
};

// One version of an item. A film ripped twice has two of these.
struct JfMediaSource {
    std::string id;
    std::string name;
    std::string container;
    int64_t     size = 0;
    int64_t     runTimeTicks = 0;
    int         bitrate = 0;
    bool        supportsDirectPlay = false;
    bool        supportsDirectStream = false;
    bool        supportsTranscoding = false;
    int         defaultAudioIndex = -1;
    int         defaultSubtitleIndex = -1;
    std::vector<JfStream> streams;

    const JfStream* streamAt(int index) const;
    std::vector<const JfStream*> ofType(const char* type) const;
};

// A named seek point.
struct JfChapter {
    int64_t     startTicks = 0;
    std::string name;
    std::string imageTag;
    int seconds() const { return (int)(startTicks / 10000000); }
};

// A cast or crew credit.
struct JfPerson {
    std::string id;
    std::string name;
    std::string role;
    std::string type;          // "Actor" | "Director" | "Writer" ...
    std::string primaryTag;
};

// One row in a library listing. Deliberately flat: the console renders from
// this directly, no second lookup.
struct JfItem {
    // Seerr fills these two; Jellyfin leaves them at their defaults and the
    // art cache builds its own URL from id and primaryTag instead.
    MediaSource source = MediaSource::Jellyfin;
    std::string artUrl;

    std::string id;
    std::string name;
    std::string type;          // "MusicAlbum" | "Audio" | "Movie" | "Series" ...
    std::string overview;
    std::string primaryTag;    // ImageTags.Primary, for cache-correct art URLs
    std::string albumArtist;
    std::string album;
    std::string albumId;
    int         productionYear = 0;
    int         indexNumber    = 0;   // track number / episode number
    int         parentIndexNumber = 0; // season number, for episodes
    int64_t     runTimeTicks   = 0;   // 10,000 ticks per millisecond
    bool        isFolder       = false;

    // Watch state, from the server rather than tracked locally, so it stays
    // in step with every other client signed into the same account.
    int64_t     resumeTicks    = 0;   // where to pick up, 0 if unwatched
    bool        played         = false;
    double      playedFraction = 0.0; // 0..1, for a progress bar on the tile

    // Episodes carry these so a list can read "S02E05" without another call.
    int         parentIndex    = 0;
    std::string seriesName;
    std::string seriesId;
    std::string seasonId;

    // Detail screen only: too much JSON to ask for on a whole grid.
    std::string genres;          // already joined, "Comedy, Romance"
    std::string studios;         // likewise
    std::string officialRating;  // "PG-13"
    double      communityRating = 0.0;
    double      criticRating = 0.0;
    std::string tagline;
    std::string premiereDate;    // ISO 8601, as the server sent it
    std::string status;          // series only: "Continuing" | "Ended"
    int         childCount = 0;  // seasons in a series, tracks in an album
    int         mediaSourceCount = 0;  // more than one means a version picker

    bool        isFavorite = false;

    // Seerr's own numbering: 0 none, 2 pending, 3 processing, 4 partial,
    // 5 available.
    int         requestStatus = 0;

    // Filled only by itemDetails, and only when asked for.
    std::vector<JfChapter> chapters;
    std::vector<JfPerson>  people;

    // Runtime in whole seconds, 0 when the server did not report one.
    int runtimeSeconds() const { return (int)(runTimeTicks / 10000000); }

    // Seconds to resume from, 0 when the item has not been started.
    int resumeSeconds() const { return (int)(resumeTicks / 10000000); }
    bool partiallyWatched() const { return resumeTicks > 0 && !played; }
};

// What a browse request is asking for. Everything past searchTerm maps
// straight onto a /Items query parameter of the same name.
struct JfQuery {
    std::string parentId;
    std::string includeItemTypes;   // e.g. "MusicAlbum" or "Audio"
    std::string sortBy = "SortName";
    std::string sortOrder = "Ascending";
    std::string searchTerm;
    bool        recursive = false;
    int         startIndex = 0;
    int         limit = 0;          // 0 => server default

    std::string filters;            // "IsUnplayed", "IsResumable", ...
    std::string genres;             // pipe separated, as the server wants
    std::string years;              // comma separated
    std::string officialRatings;    // pipe separated
    std::string studios;            // pipe separated
    std::string nameStartsWith;
    std::string mediaTypes;
    int         isFavorite = -1;    // -1 leaves it out of the query
    int         isPlayed   = -1;
    // Extra Fields to ask for, appended to the ones every listing needs.
    std::string extraFields;
};

// What a library holds, for a filter menu. /Items/Filters2 returns only these.
struct JfFilterOptions {
    std::vector<std::string> genres;
    std::vector<std::string> tags;
};

// The choices a sort menu offers.
struct JfSortOption {
    const char* label;
    const char* sortBy;
    bool        descendingByDefault;
};
const JfSortOption* JfSortOptions(int& count);

enum class QuickConnectState {
    Idle,
    WaitingForApproval,
    Approved,
    Expired,       // server dropped the request, or the user took too long
    Failed,
};

class JellyfinClient {
public:
    JellyfinClient();

    // ---- Discovery -------------------------------------------------------
    // Broadcasts on UDP 7359 and collects replies for waitMs.
    static std::vector<JfServer> Discover(int waitMs = 1500);

    // ---- Server ----------------------------------------------------------
    // Normalizes the URL (adds http:// when missing, strips trailing '/').
    void        setServerUrl(const std::string& url);
    std::string serverUrl() const { return serverUrl_; }

    // GET /System/Info/Public. Confirms the URL really is a Jellyfin server
    // and fills serverName before the user commits to signing in.
    bool ping(std::string& serverNameOut, std::string& error);

    // ---- Quick Connect ---------------------------------------------------
    // POST /QuickConnect/Initiate. On success `code` is the six characters
    // the user types into Jellyfin's web UI.
    bool quickConnectStart(std::string& code, std::string& error);

    // One poll of /QuickConnect/Connect. Call every couple of seconds.
    QuickConnectState quickConnectPoll(std::string& error);

    // Exchanges the approved secret for an access token. Only valid once
    // quickConnectPoll has returned Approved.
    bool quickConnectFinish(std::string& error);

    void quickConnectCancel();

    // Approves a Quick Connect code as the signed in user, the way the web
    // interface does when one is typed into it.
    bool authorizeQuickConnect(const std::string& code, std::string& error);

    // The server's host, without scheme or port.
    std::string serverHost() const;

    // ---- Username/password (fallback for servers with Quick Connect off) --
    bool authenticateByName(const std::string& username,
                            const std::string& password,
                            std::string& error);

    // ---- Session ---------------------------------------------------------
    bool        signedIn() const { return !accessToken_.empty(); }
    std::string userName() const { return userName_; }
    std::string userId()   const { return userId_; }
    std::string deviceId() const { return deviceId_; }

    bool loadSession();    // from DataDir()/session.json
    // A session saved for one particular server, if there is one.
    bool loadSessionFor(const std::string& serverUrl);
    // Forget the current session locally, keeping it on disk for next time.
    void releaseSession();
    bool saveSession() const;
    void signOut();        // also tells the server to drop the session

    // ---- Browse ----------------------------------------------------------
    std::vector<JfLibrary> libraries(std::string& error);
    std::vector<JfItem>    items(const JfQuery& query, std::string& error);
    // The same, also reporting how many the server holds in total.
    std::vector<JfItem>    items(const JfQuery& query, std::string& error,
                                 int& totalCount);

    // Rows for a home screen. Each is a plain list the grid can render.
    std::vector<JfItem> resumable(std::string& error, int limit = 20);
    std::vector<JfItem> nextUp(std::string& error, int limit = 20);
    std::vector<JfItem> recentlyAdded(std::string& error, int limit = 20);
    std::vector<JfItem> favorites(std::string& error, int limit = 20);
    std::vector<JfItem> search(const std::string& term, std::string& error,
                               int limit = 60);

    // Watch state. The server is the record; this just tells it.
    void markPlayed(const std::string& itemId, bool played);
    void setFavorite(const std::string& itemId, bool favorite);

    // What the items under `parentId` actually carry, for a filter menu.
    bool filterOptions(const std::string& parentId, JfFilterOptions& out,
                       std::string& error);

    // Everything a detail screen shows, chapters and cast included.
    bool itemDetailsFull(const std::string& itemId, JfItem& out,
                         std::string& error);
    bool                   itemDetails(const std::string& itemId,
                                       JfItem& out,
                                       std::string& error);

    // These endpoints rather than a ParentId query: only they return
    // broadcast order and handle specials.
    std::vector<JfItem> seasons(const std::string& seriesId, std::string& error);
    std::vector<JfItem> episodes(const std::string& seriesId,
                                 const std::string& seasonId,
                                 std::string& error);

    // ---- URLs ------------------------------------------------------------
    // Primary artwork, scaled server side. Both dimensions, so the server crops
    // and resamples to the tile's shape instead of the client rescaling.
    std::string imageUrl(const std::string& itemId,
                         const std::string& primaryTag,
                         int maxWidth,
                         int maxHeight) const;

    // 16-bit PCM in a WAV container, so no codec is needed here. About
    // 1.5 Mbit/s.
    std::string audioPcmUrl(const std::string& itemId) const;

    // Audio in its original form, no transcode. Cheaper on the network and
    // on the server, but only usable for formats we can actually decode.
    std::string audioDirectUrl(const std::string& itemId) const;

    // ---- Video playback --------------------------------------------------
    // Which version, and which tracks within it, playback should use. Left
    // alone this asks for the first version and the tracks the server would
    // pick itself.
    struct PlaybackRequest {
        std::string mediaSourceId;
        int     audioIndex    = -1;   // -1 keeps the source's own default
        int     subtitleIndex = -1;   // -1 keeps the default, -2 means none
        int64_t startTicks    = 0;
        int     maxBitrate    = 0;
    };

    // What the server intends to send us for this item, and the session id
    // that later ties a transcode and its progress reports together.
    struct PlaybackPlan {
        std::string mediaSourceId;
        std::string playSessionId;
        std::string videoCodec;
        std::string videoProfile;
        int  videoLevel   = 0;
        int  width        = 0;
        int  height       = 0;
        // How the picture should be shaped on screen, which is not always
        // the shape of the coded frame: anamorphic sources store narrow
        // pixels and rely on this to be stretched back out. Zero when the
        // server did not say, in which case the coded frame is the answer.
        double displayAspect = 0.0;
        bool directPlay   = false;

        // Every version the item has, so a picker needs no second request.
        std::vector<JfMediaSource> sources;
        int         sourceIndex   = 0;
        int         audioIndex    = -1;
        int         subtitleIndex = -1;
        std::string container;

        const JfMediaSource* source() const;
    };
    bool playbackInfo(const std::string& itemId, PlaybackPlan& out,
                      std::string& error);
    bool playbackInfo(const std::string& itemId, const PlaybackRequest& request,
                      PlaybackPlan& out, std::string& error);

    // What the viewer has asked playback to do, over and above the height.
    // Zero on any of these means the client decides.
    struct StreamPrefs {
        int videoBitrate = 0;    // kbit/s
        int maxFramerate = 0;
        int audioBitrate = 0;    // kbit/s
        std::string audioCodecs; // empty means whatever this build decodes
    };
    void setStreamPrefs(const StreamPrefs& prefs) { prefs_ = prefs; }

    // HLS master playlist for `plan`, capped at `maxHeight`.
    //
    // Asking for the source's own codec, profile and level makes the server
    // copy the video stream instead of re-encoding it, so it only has to
    // transcode the audio. Segments come back as MPEG-TS carrying H.264 in
    // Annex-B, which is what the console's decoder takes directly.
    std::string videoHlsUrl(const PlaybackPlan& plan, int maxHeight,
                            int64_t startTicks = 0) const;

    // The file itself, no server side work at all. Only usable where the
    // container and both codecs are ones this build can demux and decode.
    std::string videoDirectUrl(const PlaybackPlan& plan) const;

    // A text subtitle track as a standalone file, for drawing ourselves.
    // Bitmap tracks have no useful answer here and must be burned in.
    std::string subtitleUrl(const std::string& itemId,
                            const std::string& mediaSourceId,
                            int streamIndex,
                            const char* format = "vtt") const;

    // Fetches and parses a text subtitle track. Cues come back sorted.
    struct JfCue {
        double      start = 0.0;   // seconds
        double      end   = 0.0;
        std::string text;          // markup stripped, newlines kept
    };
    std::vector<JfCue> subtitles(const std::string& itemId,
                                 const std::string& mediaSourceId,
                                 int streamIndex,
                                 std::string& error) const;

    // Bitrate to request for a given height. Sending none lets the server
    // pick a default low enough that it scaled a 480p request down to 224p.
    static int VideoBitrateFor(int height);

    // Tells the server to stop transcoding. Without this the server keeps an
    // ffmpeg running after playback stops.
    void stopTranscode(const std::string& playSessionId);

    // ---- Playback reporting ---------------------------------------------
    // Keeps "resume" and "now playing" working on other clients.
    void reportPlaybackStart(const std::string& itemId);
    void reportPlaybackProgress(const std::string& itemId,
                                int64_t positionTicks,
                                bool paused);
    void reportPlaybackStopped(const std::string& itemId,
                               int64_t positionTicks);
    // Keeps a transcode alive while paused. The server reaps sessions that
    // stop reporting.
    void reportPlaybackPing(const std::string& playSessionId);

private:
    static std::vector<JfItem> parseItemList(const std::string& body);

    std::string authHeaderValue() const;
    std::string apiUrl(const std::string& path) const;
    // GET with the session token attached; retries `fallbackPath` on 404 so
    // one binary works across Jellyfin versions that moved an endpoint.
    std::string getJson(const std::string& path,
                        const std::string& fallbackPath,
                        std::string& error);
    void applyAuthResponse(const std::string& body);

    std::string serverUrl_;
    std::string accessToken_;
    std::string userId_;
    std::string userName_;
    std::string deviceId_;

    std::string qcSecret_;
    std::string qcCode_;
    StreamPrefs prefs_;
};
