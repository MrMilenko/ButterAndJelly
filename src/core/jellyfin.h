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

// One row in a library listing. Deliberately flat: the console renders from
// this directly, no second lookup.
struct JfItem {
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

    // Shown on the detail screen. Requested only where they will be read,
    // since asking for them on a 300 item grid is a lot of JSON for text
    // nobody sees.
    std::string genres;          // already joined, "Comedy, Romance"
    std::string officialRating;  // "PG-13"
    double      communityRating = 0.0;
    std::string tagline;

    // Runtime in whole seconds, 0 when the server did not report one.
    int runtimeSeconds() const { return (int)(runTimeTicks / 10000000); }

    // Seconds to resume from, 0 when the item has not been started.
    int resumeSeconds() const { return (int)(resumeTicks / 10000000); }
    bool partiallyWatched() const { return resumeTicks > 0 && !played; }
};

// What a browse request is asking for.
struct JfQuery {
    std::string parentId;
    std::string includeItemTypes;   // e.g. "MusicAlbum" or "Audio"
    std::string sortBy = "SortName";
    std::string sortOrder = "Ascending";
    std::string searchTerm;
    bool        recursive = false;
    int         startIndex = 0;
    int         limit = 0;          // 0 => server default
};

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
    // Broadcasts on UDP 7359 and collects replies for waitMs. Servers that
    // answer need no typing at all, which matters a lot on a console.
    static std::vector<JfServer> Discover(int waitMs = 1500);

    // ---- Server ----------------------------------------------------------
    // Normalises the URL (adds http:// when missing, strips trailing '/').
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
    bool saveSession() const;
    void signOut();        // also tells the server to drop the session

    // ---- Browse ----------------------------------------------------------
    std::vector<JfLibrary> libraries(std::string& error);
    std::vector<JfItem>    items(const JfQuery& query, std::string& error);

    // Rows for a home screen. Each is a plain list the grid can render.
    std::vector<JfItem> resumable(std::string& error, int limit = 20);
    std::vector<JfItem> nextUp(std::string& error, int limit = 20);
    std::vector<JfItem> recentlyAdded(std::string& error, int limit = 20);
    std::vector<JfItem> search(const std::string& term, std::string& error,
                               int limit = 60);

    // Watch state. The server is the record; this just tells it.
    void markPlayed(const std::string& itemId, bool played);
    bool                   itemDetails(const std::string& itemId,
                                       JfItem& out,
                                       std::string& error);

    // Series drill-down. Seasons and episodes come from dedicated endpoints
    // rather than a ParentId query, because only these return them in
    // broadcast order with the right specials handling.
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

    // Audio as 16-bit PCM in a WAV container. Asking the server to do the
    // decoding means we need no codec on a 2012 PowerPC, at the cost of
    // ~1.5 Mbit/s, which a LAN absorbs without noticing.
    std::string audioPcmUrl(const std::string& itemId) const;

    // Audio in its original form, no transcode. Cheaper on the network and
    // on the server, but only usable for formats we can actually decode.
    std::string audioDirectUrl(const std::string& itemId) const;

    // ---- Video playback --------------------------------------------------
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
    };
    bool playbackInfo(const std::string& itemId, PlaybackPlan& out,
                      std::string& error);

    // HLS master playlist for `plan`, capped at `maxHeight`.
    //
    // Asking for the source's own codec, profile and level makes the server
    // copy the video stream instead of re-encoding it, so it only has to
    // transcode the audio. Segments come back as MPEG-TS carrying H.264 in
    // Annex-B, which is what the console's decoder takes directly.
    std::string videoHlsUrl(const PlaybackPlan& plan, int maxHeight,
                            int64_t startTicks = 0) const;

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
};
