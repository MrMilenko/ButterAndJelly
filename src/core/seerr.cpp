// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/seerr.h"

#include "core/http.h"
#include "core/json.h"
#include "core/log.h"
#include "core/features.h"
#include "core/platform.h"
#include "core/thread.h"
#include "core/secret_store.h"
#include "core/text.h"

#include <cstdio>

namespace {

// TMDB serves artwork over plain HTTP, which is the only reason discovery
// works on a console with no TLS.
constexpr const char* kTmdbImageBase = "http://image.tmdb.org/t/p/";

// TMDB serves artwork over plain HTTP, which is the only reason discovery
// works on a console with no TLS.

// MediaInfo.status, from the Seerr API spec.
constexpr int kSeerrPending    = 2;
constexpr int kSeerrProcessing = 3;
constexpr int kSeerrPartial    = 4;
constexpr int kSeerrAvailable  = 5;

std::string PosterUrl(const std::string& posterPath, int targetWidth)
{
    if (posterPath.empty()) return "";
    // TMDB offers fixed widths; asking for the next one up and letting the
    // console scale down beats fetching an original on a 100Mbit link.
    const char* size = "w342";
    if      (targetWidth > 500) size = "w780";
    else if (targetWidth > 342) size = "w500";
    else if (targetWidth <= 154) size = "w154";
    else if (targetWidth <= 185) size = "w185";
    return std::string(kTmdbImageBase) + size + posterPath;
}

// Hand rolled: the 2003 CRT behind the Xbox libcxx has neither atoll nor a
// usable atoi through <cstdlib>.
long long ParseDigits(const char* text)
{
    long long value = 0;
    for (const char* p = text; p && *p >= '0' && *p <= '9'; ++p) {
        value = value * 10 + (*p - '0');
    }
    return value;
}

int YearFromDate(const std::string& date)
{
    if (date.size() < 4) return 0;
    return (int)ParseDigits(date.substr(0, 4).c_str());
}

// One result from discover or search into the shape the grid already draws.
//
// Seerr hands back the Jellyfin id for anything already in the library, so an
// available result becomes an ordinary Jellyfin item and plays like one. What
// is left is what Seerr is for: things to ask for.
bool ItemFromJson(const JsonValue& node, int posterWidth, JfItem& out)
{
    const std::string mediaType = node["mediaType"].asString();
    if (mediaType != "movie" && mediaType != "tv") return false;

    const int tmdbId = node["id"].asInt();
    if (tmdbId <= 0) return false;

    out.source = MediaSource::Seerr;
    out.id     = SeerrItemId(mediaType, tmdbId);
    out.type   = (mediaType == "movie") ? "Movie" : "Series";
    out.name   = (mediaType == "movie") ? node["title"].asString()
                                        : node["name"].asString();
    out.overview        = node["overview"].asString();
    out.artUrl          = PosterUrl(node["posterPath"].asString(), posterWidth);
    out.communityRating = node["voteAverage"].asDouble();
    out.productionYear  = YearFromDate((mediaType == "movie")
                                       ? node["releaseDate"].asString()
                                       : node["firstAirDate"].asString());

    // mediaInfo appears once Seerr knows about an item, so the tile can say
    // it is already asked for. Status values are the spec's own:
    // 1 unknown, 2 pending, 3 processing, 4 partial, 5 available, 6 deleted.
    const JsonValue info = node["mediaInfo"];
    if (info.valid()) {
        const int status = info["status"].asInt();
        const std::string jellyfinId = info["jellyfinMediaId"].asString();

        if (status == kSeerrAvailable && !jellyfinId.empty()) {
            // Hand it to Jellyfin: its own id, its own artwork, and whatever
            // watch state the library has rather than anything Seerr said.
            out.source = MediaSource::Jellyfin;
            out.id     = jellyfinId;
            out.artUrl.clear();
            return !out.name.empty();
        }

        out.requestStatus = status;
    }
    return !out.name.empty();
}

std::string ReadLine(FILE* fp)
{
    char buffer[512];
    if (!std::fgets(buffer, sizeof(buffer), fp)) return "";
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' '  || line.back() == '\t')) {
        line.pop_back();
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.erase(line.begin());
    }
    return line;
}

}  // namespace

std::string SeerrItemId(const std::string& mediaType, long long tmdbId)
{
    return "seerr:" + mediaType + ":" + bj::ToString(tmdbId);
}

bool ParseSeerrItemId(const std::string& id, std::string& mediaType,
                      long long& tmdbId)
{
    if (id.rfind("seerr:", 0) != 0) return false;
    const size_t typeStart = 6;
    const size_t colon = id.find(':', typeStart);
    if (colon == std::string::npos) return false;

    mediaType = id.substr(typeStart, colon - typeStart);
    tmdbId    = ParseDigits(id.c_str() + colon + 1);
    return tmdbId > 0 && !mediaType.empty();
}

HttpHeaders SeerrClient::authHeaders() const
{
    HttpHeaders h;
    if (!session_.empty()) {
        h.push_back({ "Cookie", session_ });
    } else if (!apiKey_.empty()) {
        h.push_back({ "X-Api-Key", apiKey_ });
    }
    h.push_back({ "Accept", "application/json" });
    return h;
}

void SeerrClient::setServer(const std::string& url)
{
    std::string u = url;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    while (!u.empty() && u.front() == ' ') u.erase(u.begin());
    if (!u.empty() && u.find("://") == std::string::npos) u = "http://" + u;
    baseUrl_ = u;
}

bool SeerrClient::reachable(std::string& error)
{
    if (baseUrl_.empty()) { error = "No address"; return false; }

    HttpResponse response = Http::Get(apiUrl("/status"), HttpHeaders());
    if (!response.ok()) {
        error = response.error.empty() ? "Nothing answered at that address"
                                       : response.error;
        return false;
    }
    return true;
}

bool SeerrClient::quickConnectStart(std::string& code, std::string& error)
{
    qcSecret_.clear();
    if (baseUrl_.empty()) { error = "No address"; return false; }

    HttpResponse response =
        Http::Post(apiUrl("/auth/jellyfin/quickconnect/initiate"), "{}",
                   HttpHeaders{ { "Content-Type", "application/json" } });
    if (!response.ok()) {
        error = "The request server would not start Quick Connect";
        return false;
    }

    JsonDoc doc(response.body);
    code      = doc["code"].asString();
    qcSecret_ = doc["secret"].asString();
    if (code.empty() || qcSecret_.empty()) {
        error = "The request server sent no code";
        return false;
    }
    return true;
}

bool SeerrClient::quickConnectApproved(std::string& error)
{
    if (qcSecret_.empty()) { error = "Not waiting on a code"; return false; }

    HttpResponse response =
        Http::Get(apiUrl("/auth/jellyfin/quickconnect/check?secret=" +
                         Http::UrlEncode(qcSecret_)), HttpHeaders());
    if (response.status == 404) {
        error = "The code expired";
        qcSecret_.clear();
        return false;
    }
    if (!response.ok()) return false;

    JsonDoc doc(response.body);
    return doc["authenticated"].asBool(false);
}

bool SeerrClient::quickConnectFinish(std::string& error)
{
    if (qcSecret_.empty()) { error = "Not waiting on a code"; return false; }

    const std::string body = "{\"secret\":\"" + qcSecret_ + "\"}";
    HttpResponse response =
        Http::Post(apiUrl("/auth/jellyfin/quickconnect/authenticate"), body,
                   HttpHeaders{ { "Content-Type", "application/json" } });
    if (!response.ok() || response.setCookie.empty()) {
        error = response.status == 403
              ? "The request server refused that account"
              : "The request server would not finish signing in";
        return false;
    }

    session_ = response.setCookie;
    qcSecret_.clear();

    JsonDoc doc(response.body);
    userName_ = doc["displayName"].asString();
    if (userName_.empty()) userName_ = doc["jellyfinUsername"].asString();
    if (userName_.empty()) userName_ = doc["username"].asString();

    LOGF("[seerr] signed in as %s", userName_.c_str());
    saveConfig();
    return true;
}

namespace {

// A JSON string body, with the few characters that would break it escaped.
std::string JsonQuoted(const std::string& text)
{
    std::string out = "\"";
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if ((unsigned char)ch < 0x20)  out += ' ';
        else out += ch;
    }
    out += '"';
    return out;
}

}  // namespace

bool SeerrClient::signInWithPassword(const std::string& user,
                                     const std::string& password,
                                     std::string& error, bool persist)
{
    if (!reachable(error)) return false;
    if (user.empty() || password.empty()) {
        error = "A user name and password are needed";
        return false;
    }

    const HttpHeaders headers = { { "Content-Type", "application/json" } };

    struct Attempt { const char* path; const char* nameField; };
    const Attempt attempts[] = {
        { "/auth/jellyfin", "username" },
        { "/auth/local",    "email"    },
    };

    for (const Attempt& attempt : attempts) {
        const std::string body = std::string("{") +
            JsonQuoted(attempt.nameField) + ":" + JsonQuoted(user) + "," +
            JsonQuoted("password") + ":" + JsonQuoted(password) + "}";

        HttpResponse response = Http::Post(apiUrl(attempt.path), body, headers);
        if (!response.ok() || response.setCookie.empty()) continue;

        session_ = response.setCookie;
        JsonDoc doc(response.body);
        userName_ = doc["displayName"].asString();
        if (userName_.empty()) userName_ = doc["jellyfinUsername"].asString();
        if (userName_.empty()) userName_ = user;

        LOGF("[seerr] signed in as %s", userName_.c_str());
        if (persist) saveConfig();
        return true;
    }

    error = "That user name and password were refused";
    return false;
}

std::string SeerrClient::GuessAddressFor(const JellyfinClient& jellyfin)
{
    const std::string host = jellyfin.serverHost();
    if (host.empty()) return "";
    // 5055 is the port Seerr ships with.
    return "http://" + host + ":5055";
}

bool SeerrClient::signInWithJellyfin(JellyfinClient& jellyfin, std::string& error)
{
    if (!jellyfin.signedIn()) { error = "Not signed in to Jellyfin"; return false; }
    if (!reachable(error)) return false;

    std::string code;
    if (!quickConnectStart(code, error)) return false;

    // Already signed in here, so the client answers its own code.
    if (!jellyfin.authorizeQuickConnect(code, error)) {
        quickConnectCancel();
        return false;
    }

    // Seerr polls its own Jellyfin for the approval.
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::string pollError;
        if (quickConnectApproved(pollError)) return quickConnectFinish(error);
        if (!pollError.empty()) { error = pollError; return false; }
        bj::SleepMs(500);
    }

    error = "The request server did not see the approval";
    quickConnectCancel();
    return false;
}

void SeerrClient::signOut()
{
    if (!session_.empty()) {
        Http::Post(apiUrl("/auth/logout"), "", authHeaders());
    }
    session_.clear();
    userName_.clear();
    qcSecret_.clear();
    saveConfig();
}

namespace {

// One file per Jellyfin server, named the way its session is.
std::string SeerrSessionPath(const std::string& pairedWith)
{
    if (pairedWith.empty()) return Platform::DataDir() + "/seerr.json";

    std::string key;
    key.reserve(pairedWith.size());
    for (char ch : pairedWith) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9');
        key += safe ? ch : '_';
    }
    return Platform::DataDir() + "/seerr_" + key + ".json";
}

}  // namespace

bool SeerrClient::saveConfig() const
{
    FILE* fp = std::fopen(Platform::NativePath(SeerrSessionPath(pairedWith_)).c_str(), "wb");
    if (!fp) return false;

    // Obfuscated the same way the Jellyfin token is.
    std::fprintf(fp,
        "{\n  \"url\": \"%s\",\n  \"session\": \"%s\",\n"
        "  \"user\": \"%s\"\n}\n",
        baseUrl_.c_str(), Secret::Hide(session_).c_str(), userName_.c_str());
    std::fclose(fp);
    return true;
}

// A session this app signed in for, if there is one.
bool SeerrClient::loadSession()
{
    FILE* fp = std::fopen(Platform::NativePath(SeerrSessionPath(pairedWith_)).c_str(), "rb");
    if (!fp) return false;

    std::string text;
    char buffer[512];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), fp)) > 0) text.append(buffer, n);
    std::fclose(fp);

    JsonDoc doc(text);
    if (!doc.valid()) return false;

    const std::string url = doc["url"].asString();
    const std::string session = Secret::Show(doc["session"].asString());
    if (url.empty() || session.empty()) return false;

    baseUrl_  = url;
    session_  = session;
    userName_ = doc["user"].asString();
    LOGF("[seerr] session restored for %s", baseUrl_.c_str());
    return true;
}

void SeerrClient::pairWith(const std::string& jellyfinServerUrl)
{
    if (pairedWith_ == jellyfinServerUrl) return;

    // A different library means a different Seerr.
    pairedWith_ = jellyfinServerUrl;
    baseUrl_.clear();
    apiKey_.clear();
    session_.clear();
    userName_.clear();
    qcSecret_.clear();
    loadConfig();
}

bool SeerrClient::loadConfig()
{
    // A session the viewer signed in for wins: it is the one they chose.
    if (loadSession()) return true;

    const std::string candidates[] = {
        Platform::DataDir()  + "/seerr.txt",
        Platform::AssetDir() + "/seerr.txt",
    };

    FILE* fp = nullptr;
    for (const std::string& candidate : candidates) {
        fp = std::fopen(Platform::NativePath(candidate).c_str(), "r");
        if (fp) break;
    }
    if (!fp) return false;

    // First non-comment line is the URL, the second is the key.
    std::string url, key;
    while (url.empty() || key.empty()) {
        const std::string line = ReadLine(fp);
        if (line.empty() && std::feof(fp)) break;
        if (line.empty() || line[0] == '#') continue;
        if (url.empty()) url = line; else key = line;
    }
    std::fclose(fp);

    if (url.empty() || key.empty()) return false;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.find("://") == std::string::npos) url = "http://" + url;

    baseUrl_ = url;
    apiKey_  = key;
    LOGF("[seerr] configured for %s", baseUrl_.c_str());
    return true;
}

std::string SeerrClient::apiUrl(const std::string& path) const
{
    return baseUrl_ + "/api/v1" + path;
}

bool SeerrClient::ping(std::string& error)
{
    if (!configured()) { error = "No request server configured"; return false; }

    const HttpHeaders headers = authHeaders();
    HttpResponse response = Http::Get(apiUrl("/status"), headers);
    if (!response.ok()) {
        error = response.error.empty() ? "The request server did not answer" : response.error;
        return false;
    }
    return true;
}

std::vector<JfItem> SeerrClient::discover(const char* what, int page,
                                          std::string& error)
{
    Page ignored;
    return discover(what, page, error, ignored);
}

std::vector<JfItem> SeerrClient::discover(const char* what, int page,
                                          std::string& error, Page& info)
{
    std::vector<JfItem> out;
    if (!configured()) { error = "No request server configured"; return out; }

    const HttpHeaders headers = authHeaders();
    // `what` is the path under /discover, and may already carry a query:
    // filtering is by parameter in this API, as in "movies?genre=28".
    std::string url = apiUrl("/discover/" + std::string(what));
    url += (url.find('?') == std::string::npos ? "?page=" : "&page=") +
           bj::ToString(page);

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        error = response.error.empty() ? "Could not reach the request server" : response.error;
        LOGF("[seerr] discover %s page %d: %ld %s", what, page,
             (long)response.status, response.body.c_str());
        return out;
    }

    JsonDoc doc(response.body);
    if (!doc.valid()) { error = "The request server sent something unreadable"; return out; }

    info.page         = doc["page"].asInt(page);
    info.totalPages   = doc["totalPages"].asInt(1);
    info.totalResults = doc["totalResults"].asInt(0);

    const JsonValue results = doc["results"];
    for (int i = 0; i < results.size(); ++i) {
        JfItem item;
        if (!ItemFromJson(results.at(i), BJ_POSTER_WIDTH, item)) continue;
        // Anything already in the library is not a discovery.
        // Already in the library, or already asked for, belongs elsewhere.
        if (item.source == MediaSource::Jellyfin) continue;
        if (item.requestStatus > 0) continue;
        out.push_back(item);
    }
    return out;
}

std::vector<JfItem> SeerrClient::search(const std::string& query,
                                        std::string& error)
{
    std::vector<JfItem> out;
    if (!configured()) { error = "No request server configured"; return out; }
    if (query.empty()) return out;

    const HttpHeaders headers = authHeaders();
    const std::string url = apiUrl("/search") + "?query=" + Http::UrlEncode(query);

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        error = response.error.empty() ? "Could not reach the request server" : response.error;
        return out;
    }

    JsonDoc doc(response.body);
    if (!doc.valid()) { error = "The request server sent something unreadable"; return out; }

    const JsonValue results = doc["results"];
    for (int i = 0; i < results.size(); ++i) {
        JfItem item;
        if (ItemFromJson(results.at(i), BJ_POSTER_WIDTH, item)) out.push_back(item);
    }
    return out;
}

namespace { const char* TmdbBase() { return kTmdbImageBase; } }

std::string SeerrPathId(const std::string& path) { return "seerrpath:" + path; }

bool ParseSeerrPathId(const std::string& id, std::string& path)
{
    if (id.rfind("seerrpath:", 0) != 0) return false;
    path = id.substr(10);
    return !path.empty();
}

std::vector<JfItem> SeerrClient::genres(const char* mediaType, std::string& error)
{
    std::vector<JfItem> out;
    if (!configured()) { error = "No request server configured"; return out; }

    const HttpHeaders headers = authHeaders();
    const std::string url = apiUrl("/discover/genreslider/" + std::string(mediaType));

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        error = response.error.empty() ? "Could not reach the request server" : response.error;
        return out;
    }

    JsonDoc doc(response.body);
    if (!doc.valid()) { error = "The request server sent something unreadable"; return out; }

    // The reply is a bare array rather than an object with results.
    const JsonValue list = doc.root();
    const std::string plural = (std::string(mediaType) == "movie") ? "movies" : "tv";

    for (int i = 0; i < list.size(); ++i) {
        const JsonValue node = list.at(i);
        const int id = node["id"].asInt();
        const std::string name = node["name"].asString();
        if (id <= 0 || name.empty()) continue;

        JfItem item;
        item.source   = MediaSource::Seerr;
        item.id       = SeerrPathId(plural + "?genre=" + bj::ToString(id));
        item.name     = name;
        item.type     = "Genre";
        item.isFolder = true;

        // Genre art is a backdrop, so these tiles are wide rather than poster
        // shaped. The first one is enough.
        const JsonValue backdrops = node["backdrops"];
        if (backdrops.size() > 0) {
            item.artUrl = std::string(TmdbBase()) +
                          (BJ_POSTER_WIDTH > 342 ? "w780" : "w300") +
                          backdrops.at(0).asString();
        }
        out.push_back(item);
    }
    return out;
}

bool SeerrClient::title(const std::string& mediaType, long long tmdbId,
                        JfItem& out, std::string& error)
{
    if (!configured()) { error = "No request server configured"; return false; }
    if (mediaType != "movie" && mediaType != "tv") return false;

    const HttpHeaders headers = authHeaders();
    const std::string url = apiUrl("/" + mediaType + "/" + bj::ToString(tmdbId));

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        error = response.error.empty() ? "Could not reach the request server"
                                       : response.error;
        return false;
    }

    JsonDoc doc(response.body);
    if (!doc.valid()) { error = "The request server sent something unreadable"; return false; }

    // A title fetched on its own carries no mediaType, so it is supplied.
    JsonValue node = doc.root();
    JfItem item;
    item.source = MediaSource::Seerr;
    item.id     = SeerrItemId(mediaType, (int)tmdbId);
    item.type   = (mediaType == "movie") ? "Movie" : "Series";
    item.name   = (mediaType == "movie") ? node["title"].asString()
                                         : node["name"].asString();
    item.overview        = node["overview"].asString();
    item.artUrl          = PosterUrl(node["posterPath"].asString(), BJ_POSTER_WIDTH);
    item.communityRating = node["voteAverage"].asDouble();
    item.productionYear  = YearFromDate((mediaType == "movie")
                                        ? node["releaseDate"].asString()
                                        : node["firstAirDate"].asString());

    item.tagline = node["tagline"].asString();
    {
        std::string joined;
        for (const JsonValue& genre : node["genres"].items()) {
            const std::string name = genre.str("name");
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name;
        }
        item.genres = joined;
    }
    {
        std::string joined;
        const JsonValue studios = (mediaType == "movie") ? node["productionCompanies"]
                                                         : node["networks"];
        for (const JsonValue& studio : studios.items()) {
            const std::string name = studio.str("name");
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name;
            if (joined.size() > 60) break;
        }
        item.studios = joined;
    }

    // Films give a runtime in minutes; shows give the length of an episode.
    int minutes = node["runtime"].asInt(0);
    if (minutes <= 0) {
        const JsonValue runtimes = node["episodeRunTime"];
        if (runtimes.size() > 0) minutes = runtimes.at(0).asInt(0);
    }
    if (minutes > 0) item.runTimeTicks = (int64_t)minutes * 60 * 10000000;

    for (const JsonValue& person : node["credits"]["cast"].items()) {
        JfPerson credit;
        credit.name = person.str("name");
        credit.role = person.str("character");
        credit.type = "Actor";
        if (!credit.name.empty()) item.people.push_back(credit);
        if (item.people.size() >= 8) break;
    }

    const JsonValue info = node["mediaInfo"];
    if (info.valid()) {
        const int status = info["status"].asInt();
        const std::string jellyfinId = info["jellyfinMediaId"].asString();
        if (status == kSeerrAvailable && !jellyfinId.empty()) {
            item.source = MediaSource::Jellyfin;
            item.id     = jellyfinId;
            item.artUrl.clear();
        } else {
            item.requestStatus = status;
        }
    }

    out = item;
    return !out.name.empty();
}

std::vector<JfItem> SeerrClient::requests(const char* filter, std::string& error,
                                          int limit)
{
    Page ignored;
    return requests(filter, 1, limit, error, ignored);
}

std::vector<JfItem> SeerrClient::requests(const char* filter, int page, int limit,
                                          std::string& error, Page& info)
{
    std::vector<JfItem> out;
    if (!configured()) { error = "No request server configured"; return out; }
    if (page < 1) page = 1;

    const HttpHeaders headers = authHeaders();
    std::string url = apiUrl("/request?take=") + bj::ToString(limit) +
                      "&skip=" + bj::ToString((page - 1) * limit) +
                      "&sort=added&sortDirection=desc";
    if (filter && *filter) url += "&filter=" + std::string(filter);

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        error = response.error.empty() ? "Could not reach the request server"
                                       : response.error;
        return out;
    }

    JsonDoc doc(response.body);
    if (!doc.valid()) { error = "The request server sent something unreadable"; return out; }

    const JsonValue info_ = doc["pageInfo"];
    info.page         = info_["page"].asInt(page);
    info.totalPages   = info_["pages"].asInt(1);
    info.totalResults = info_["results"].asInt(0);

    const JsonValue results = doc["results"];
    for (int i = 0; i < results.size(); ++i) {
        const JsonValue media = results.at(i)["media"];
        const int tmdbId = media["tmdbId"].asInt();
        const std::string mediaType = media["mediaType"].asString();
        if (tmdbId <= 0) continue;

        JfItem item;
        std::string itemError;
        if (title(mediaType, tmdbId, item, itemError)) out.push_back(item);
    }

    if (out.empty() && error.empty() && page == 1) {
        error = "Nothing has been requested yet";
    }
    return out;
}

bool SeerrClient::request(const JfItem& item, std::string& error)
{
    if (!configured()) { error = "No request server configured"; return false; }

    std::string mediaType;
    long long tmdbId = 0;
    if (!ParseSeerrItemId(item.id, mediaType, tmdbId)) {
        error = "That item cannot be requested";
        return false;
    }

    HttpHeaders headers = authHeaders();
    headers.push_back({ "Content-Type", "application/json" });

    // A show with no seasons named asks for all of them, which is what
    // someone pressing Request on a series from a sofa means.
    std::string body = "{\"mediaType\":\"" + mediaType + "\",\"mediaId\":" +
                       bj::ToString(tmdbId);
    if (mediaType == "tv") body += ",\"seasons\":\"all\"";
    body += "}";

    HttpResponse response = Http::Post(apiUrl("/request"), body, headers);
    if (!response.ok()) {
        // 409 is Seerr saying it already has this one.
        if (response.status == 409) { error = "Already requested"; return false; }
        if (response.status == 403) {
            // A Jellyfin account imported into Seerr starts with no
            // permissions at all, so this is the usual first refusal.
            error = "This account may not make requests yet";
            return false;
        }
        if (!response.error.empty()) { error = response.error; return false; }
        error = "The request was refused (" +
                bj::ToString((long long)response.status) + ")";
        LOGF("[seerr] request refused %ld: %s", (long)response.status,
             response.body.c_str());
        return false;
    }
    return true;
}
