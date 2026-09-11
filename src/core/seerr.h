// SPDX-License-Identifier: GPL-2.0-or-later

// seerr.h: discovery and requests through a Seerr server.
//
// Seerr holds what the library does not: things to browse and ask for rather
// than play. Items come back shaped like Jellyfin's so the same grid, art
// cache and detail screen draw them, with source set to Seerr so the UI
// offers Request where it would otherwise offer Play.
//
// Signs in as the Jellyfin account through Seerr's own Quick Connect, or
// with a user name and password, or from an API key in seerr.txt.

#pragma once

#include <string>
#include <vector>

#include "core/http.h"
#include "core/jellyfin.h"

class SeerrClient {
public:
    // Seerr is stored per Jellyfin server, so the two travel as a pair.
    void pairWith(const std::string& jellyfinServerUrl);

    // The saved session for the paired server, then seerr.txt as a fallback.
    bool loadConfig();
    bool configured() const {
        return !baseUrl_.empty() && (!apiKey_.empty() || !session_.empty());
    }

    const std::string& baseUrl() const { return baseUrl_; }

    // Adds http:// when missing, drops a trailing slash.
    void setServer(const std::string& url);

    // Reachable, before anything is signed in.
    bool reachable(std::string& error);

    // ---- Quick Connect ---------------------------------------------------
    // Seerr runs this against the same Jellyfin, so one code signs in both.
    bool quickConnectStart(std::string& code, std::string& error);
    // One poll. True once the code has been approved.
    bool quickConnectApproved(std::string& error);
    // Exchanges the approved secret for a session, and saves it.
    bool quickConnectFinish(std::string& error);
    void quickConnectCancel() { qcSecret_.clear(); }

    // Signs in with no interaction: `jellyfin` approves the code itself.
    bool signInWithJellyfin(JellyfinClient& jellyfin, std::string& error);

    // Tries a Jellyfin account first, then a local Seerr one.
    // `persist` off leaves the saved session alone.
    bool signInWithPassword(const std::string& user, const std::string& password,
                            std::string& error, bool persist = true);

    // The address the request server usually sits at when it runs beside a
    // Jellyfin server. Empty when there is no Jellyfin server to go by.
    static std::string GuessAddressFor(const JellyfinClient& jellyfin);

    bool signedIn() const { return !session_.empty(); }
    std::string userName() const { return userName_; }
    void signOut();

    bool saveConfig() const;
    // Just the saved session, without the seerr.txt fallback.
    bool loadSession();

    // Reachable and the key accepted.
    bool ping(std::string& error);

    // Where a listing sits in a longer one. Results are what the server
    // held, before anything already in the library was dropped.
    struct Page {
        int page = 1;
        int totalPages = 1;
        int totalResults = 0;
        bool more() const { return page < totalPages; }
    };

    // Trending and popular, for the rows on the discovery screen.
    std::vector<JfItem> discover(const char* what, int page, std::string& error);
    std::vector<JfItem> discover(const char* what, int page, std::string& error,
                                 Page& info);

    // Multi search across films and shows.
    std::vector<JfItem> search(const std::string& query, std::string& error);

    // The genre list, as browsable tiles with a backdrop each. mediaType is
    // "movie" or "tv".
    std::vector<JfItem> genres(const char* mediaType, std::string& error);

    // Asks for an item. tmdbId and mediaType come from the item's own id.
    bool request(const JfItem& item, std::string& error);

    // What has been asked for. `filter` is the API's own vocabulary:
    // "all", "pending", "approved", "processing", "available".
    // A request only carries an id, so each one is looked up in turn to get
    // a title and a poster.
    std::vector<JfItem> requests(const char* filter, std::string& error,
                                 int limit = 30);
    std::vector<JfItem> requests(const char* filter, int page, int limit,
                                 std::string& error, Page& info);

    // One title by its TMDB id. mediaType is "movie" or "tv".
    bool title(const std::string& mediaType, long long tmdbId, JfItem& out,
               std::string& error);

private:
    std::string apiUrl(const std::string& path) const;
    // A session cookie when there is one, the API key otherwise.
    HttpHeaders authHeaders() const;

    std::string baseUrl_;
    std::string apiKey_;
    std::string session_;    // "connect.sid=..." as the server handed it back
    std::string userName_;
    std::string qcSecret_;
    std::string pairedWith_;   // the Jellyfin server URL this belongs to
};

// "seerr:movie:12345" packs what a request needs into JfItem::id, since the
// grid and detail screen only ever carry the one identifier around.
std::string SeerrItemId(const std::string& mediaType, long long tmdbId);
bool ParseSeerrItemId(const std::string& id, std::string& mediaType,
                      long long& tmdbId);

// "seerrpath:movies/genre/28" is a browsable discover path rather than an
// item. Opening one lists what it holds.
std::string SeerrPathId(const std::string& path);
bool ParseSeerrPathId(const std::string& id, std::string& path);
