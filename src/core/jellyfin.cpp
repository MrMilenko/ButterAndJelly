// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/jellyfin.h"

#include "core/http.h"
#include "core/json.h"
#include "core/platform.h"
#include "core/text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// Jellyfin inherited Emby's Authorization header: one header carrying
// comma-separated key="value" pairs rather than a bearer token.
std::string BuildAuthHeader(const std::string& deviceId,
                            const std::string& token)
{
    std::string h = "MediaBrowser Client=\"";
    h += kAppName;
    h += "\", Device=\"";
    h += kPlatformLongName;
    h += "\", DeviceId=\"";
    h += deviceId;
    h += "\", Version=\"";
    h += kAppVersion;
    h += "\"";
    if (!token.empty()) {
        h += ", Token=\"";
        h += token;
        h += "\"";
    }
    return h;
}

std::string GenerateDeviceId()
{
    static bool seeded = false;
    if (!seeded) {
        std::srand((unsigned)(std::time(nullptr) ^ (uintptr_t)&seeded));
        seeded = true;
    }
    static const char* kHex = "0123456789abcdef";
    std::string id;
    id.reserve(36);
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) id += '-';
        id += kHex[std::rand() & 0x0F];
    }
    return id;
}

std::string SessionPath()
{
    return Platform::DataDir() + "/session.json";
}

std::string ReadWholeFile(const std::string& path)
{
    FILE* fp = std::fopen(Platform::NativePath(path).c_str(), "rb");
    if (!fp) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}

JfItem ParseItem(const JsonValue& obj)
{
    JfItem it;
    it.id             = obj.str("Id");
    it.name           = obj.str("Name");
    it.type           = obj.str("Type");
    it.overview       = obj.str("Overview");
    it.albumArtist    = obj.str("AlbumArtist");
    it.album          = obj.str("Album");
    it.albumId        = obj.str("AlbumId");
    it.productionYear = obj.num("ProductionYear");
    it.indexNumber    = obj.num("IndexNumber");
    it.parentIndexNumber = obj.num("ParentIndexNumber");
    it.isFolder       = obj.flag("IsFolder");
    it.primaryTag     = obj["ImageTags"].str("Primary");
    // RunTimeTicks overflows a 32-bit int for anything over ~3.5 minutes,
    // so read it as a double and widen rather than going through asInt.
    it.runTimeTicks   = (int64_t)obj["RunTimeTicks"].asDouble(0.0);
    it.parentIndex    = obj.num("ParentIndexNumber");
    it.seriesName     = obj.str("SeriesName");
    it.seriesId       = obj.str("SeriesId");
    it.seasonId       = obj.str("SeasonId");

    // Watch state lives on the server so every client agrees about it.
    it.officialRating  = obj.str("OfficialRating");
    it.communityRating = obj["CommunityRating"].asDouble(0.0);
    it.tagline         = obj["Taglines"].size() > 0
                       ? obj["Taglines"].at(0).asString() : "";
    {
        std::string joined;
        for (const JsonValue& genre : obj["Genres"].items()) {
            const std::string name = genre.asString();
            if (name.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += name;
        }
        it.genres = joined;
    }

    JsonValue userData = obj["UserData"];
    if (userData.valid()) {
        it.resumeTicks    = (int64_t)userData["PlaybackPositionTicks"].asDouble(0.0);
        it.played         = userData.flag("Played");
        it.playedFraction = userData["PlayedPercentage"].asDouble(0.0) / 100.0;
        if (it.playedFraction <= 0.0 && it.runTimeTicks > 0 && it.resumeTicks > 0) {
            it.playedFraction = (double)it.resumeTicks / (double)it.runTimeTicks;
        }
    }
    return it;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

JellyfinClient::JellyfinClient()
{
    Platform::Init();
    deviceId_ = GenerateDeviceId();
}

void JellyfinClient::setServerUrl(const std::string& url)
{
    std::string u = url;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    while (!u.empty() && u.front() == ' ') u.erase(u.begin());
    if (!u.empty() && u.find("://") == std::string::npos) u = "http://" + u;
    serverUrl_ = u;
}

std::string JellyfinClient::apiUrl(const std::string& path) const
{
    return serverUrl_ + path;
}

std::string JellyfinClient::authHeaderValue() const
{
    return BuildAuthHeader(deviceId_, accessToken_);
}

// ------------------------------------------------------------------ helpers

static HttpHeaders HeadersFor(const std::string& authValue)
{
    HttpHeaders h;
    h.push_back({ "Accept", "application/json" });
    h.push_back({ "Content-Type", "application/json" });
    h.push_back({ "Authorization", authValue });
    return h;
}

std::string JellyfinClient::getJson(const std::string& path,
                                    const std::string& fallbackPath,
                                    std::string& error)
{
    if (serverUrl_.empty()) { error = "No server selected"; return ""; }

    HttpResponse r = Http::Get(apiUrl(path), HeadersFor(authHeaderValue()));
    if (r.status == 404 && !fallbackPath.empty()) {
        r = Http::Get(apiUrl(fallbackPath), HeadersFor(authHeaderValue()));
    }
    if (!r.ok()) {
        if (!r.error.empty()) {
            error = r.error;
        } else if (r.status == 401) {
            error = "Session expired - sign in again";
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Server returned %ld", r.status);
            error = buf;
        }
        return "";
    }
    return r.body;
}

// --------------------------------------------------------------------- ping

bool JellyfinClient::ping(std::string& serverNameOut, std::string& error)
{
    if (serverUrl_.empty()) { error = "No server address"; return false; }

    HttpResponse r = Http::Get(apiUrl("/System/Info/Public"),
                               HeadersFor(authHeaderValue()));
    if (!r.ok()) {
        error = r.error.empty() ? "Could not reach that address" : r.error;
        return false;
    }
    JsonDoc doc(r.body);
    if (!doc.valid()) { error = "That address is not a Jellyfin server"; return false; }

    serverNameOut = doc["ServerName"].asString();
    if (serverNameOut.empty()) serverNameOut = "Jellyfin";
    return true;
}

// ------------------------------------------------------------ Quick Connect

bool JellyfinClient::quickConnectStart(std::string& code, std::string& error)
{
    qcSecret_.clear();
    qcCode_.clear();
    if (serverUrl_.empty()) { error = "No server selected"; return false; }

    HttpResponse r = Http::Post(apiUrl("/QuickConnect/Initiate"), "",
                                HeadersFor(authHeaderValue()));
    if (r.status == 401 || r.status == 403) {
        error = "Quick Connect is disabled on this server";
        return false;
    }
    if (!r.ok()) {
        error = r.error.empty() ? "Could not start Quick Connect" : r.error;
        return false;
    }

    JsonDoc doc(r.body);
    qcCode_   = doc["Code"].asString();
    qcSecret_ = doc["Secret"].asString();
    if (qcCode_.empty() || qcSecret_.empty()) {
        error = "Quick Connect gave an unexpected reply";
        return false;
    }
    code = qcCode_;
    return true;
}

QuickConnectState JellyfinClient::quickConnectPoll(std::string& error)
{
    if (qcSecret_.empty()) return QuickConnectState::Idle;

    const std::string url = apiUrl("/QuickConnect/Connect?Secret=" +
                                   Http::UrlEncode(qcSecret_));
    HttpResponse r = Http::Get(url, HeadersFor(authHeaderValue()));

    // The server forgets a request once it is used or times out. Both show
    // up as 404, and both mean "start over".
    if (r.status == 404) {
        qcSecret_.clear();
        qcCode_.clear();
        return QuickConnectState::Expired;
    }
    if (!r.ok()) {
        // A single dropped poll on console wifi is normal; stay in the loop
        // and let the caller's overall deadline decide when to give up.
        error = r.error.empty() ? "Waiting for the server" : r.error;
        return QuickConnectState::WaitingForApproval;
    }

    JsonDoc doc(r.body);
    if (doc["Authenticated"].asBool()) return QuickConnectState::Approved;
    return QuickConnectState::WaitingForApproval;
}

bool JellyfinClient::quickConnectFinish(std::string& error)
{
    if (qcSecret_.empty()) { error = "Nothing to finish"; return false; }

    const std::string body = "{\"Secret\":\"" + JsonEscape(qcSecret_) + "\"}";
    HttpResponse r = Http::Post(apiUrl("/Users/AuthenticateWithQuickConnect"),
                                body, HeadersFor(authHeaderValue()));
    if (!r.ok()) {
        error = r.error.empty() ? "Sign-in was rejected" : r.error;
        return false;
    }

    applyAuthResponse(r.body);
    qcSecret_.clear();
    qcCode_.clear();

    if (accessToken_.empty() || userId_.empty()) {
        error = "Sign-in reply was missing the token";
        return false;
    }
    saveSession();
    return true;
}

void JellyfinClient::quickConnectCancel()
{
    qcSecret_.clear();
    qcCode_.clear();
}

// ------------------------------------------------------- password sign-in

bool JellyfinClient::authenticateByName(const std::string& username,
                                        const std::string& password,
                                        std::string& error)
{
    if (serverUrl_.empty()) { error = "No server selected"; return false; }

    const std::string body =
        "{\"Username\":\"" + JsonEscape(username) +
        "\",\"Pw\":\"" + JsonEscape(password) + "\"}";

    HttpResponse r = Http::Post(apiUrl("/Users/AuthenticateByName"), body,
                                HeadersFor(authHeaderValue()));
    if (r.status == 401) { error = "Wrong username or password"; return false; }
    if (!r.ok()) {
        error = r.error.empty() ? "Sign-in failed" : r.error;
        return false;
    }

    applyAuthResponse(r.body);
    if (accessToken_.empty()) { error = "Sign-in reply was missing the token"; return false; }
    saveSession();
    return true;
}

void JellyfinClient::applyAuthResponse(const std::string& body)
{
    JsonDoc doc(body);
    accessToken_ = doc["AccessToken"].asString();
    JsonValue user = doc["User"];
    userId_   = user.str("Id");
    userName_ = user.str("Name");
}

// ------------------------------------------------------------------ session

bool JellyfinClient::saveSession() const
{
    const std::string path = SessionPath();
    FILE* fp = std::fopen(Platform::NativePath(path).c_str(), "wb");
    if (!fp) return false;

    std::fprintf(fp,
        "{\n"
        "  \"serverUrl\": \"%s\",\n"
        "  \"accessToken\": \"%s\",\n"
        "  \"userId\": \"%s\",\n"
        "  \"userName\": \"%s\",\n"
        "  \"deviceId\": \"%s\"\n"
        "}\n",
        JsonEscape(serverUrl_).c_str(),
        JsonEscape(accessToken_).c_str(),
        JsonEscape(userId_).c_str(),
        JsonEscape(userName_).c_str(),
        JsonEscape(deviceId_).c_str());

    std::fclose(fp);
    return true;
}

bool JellyfinClient::loadSession()
{
    const std::string text = ReadWholeFile(SessionPath());
    if (text.empty()) return false;

    JsonDoc doc(text);
    if (!doc.valid()) return false;

    serverUrl_   = doc["serverUrl"].asString();
    accessToken_ = doc["accessToken"].asString();
    userId_      = doc["userId"].asString();
    userName_    = doc["userName"].asString();

    // Reuse the stored device id so the server keeps one entry for this
    // console instead of adding a new device on every launch.
    const std::string storedDevice = doc["deviceId"].asString();
    if (!storedDevice.empty()) deviceId_ = storedDevice;

    return !accessToken_.empty() && !serverUrl_.empty();
}

void JellyfinClient::signOut()
{
    if (!accessToken_.empty() && !serverUrl_.empty()) {
        // Best effort: if the console is offline the local state still clears.
        Http::Post(apiUrl("/Sessions/Logout"), "", HeadersFor(authHeaderValue()));
    }
    accessToken_.clear();
    userId_.clear();
    userName_.clear();
    qcSecret_.clear();
    qcCode_.clear();
    std::remove(Platform::NativePath(SessionPath()).c_str());
}

// ------------------------------------------------------------------- browse

std::vector<JfLibrary> JellyfinClient::libraries(std::string& error)
{
    std::vector<JfLibrary> out;
    if (userId_.empty()) { error = "Not signed in"; return out; }

    const std::string body = getJson("/UserViews?userId=" + Http::UrlEncode(userId_),
                                     "/Users/" + Http::UrlEncode(userId_) + "/Views",
                                     error);
    if (body.empty()) return out;

    JsonDoc doc(body);
    for (const JsonValue& obj : doc["Items"].items()) {
        JfLibrary lib;
        lib.id             = obj.str("Id");
        lib.name           = obj.str("Name");
        lib.collectionType = obj.str("CollectionType");
        if (!lib.id.empty()) out.push_back(lib);
    }
    return out;
}

std::vector<JfItem> JellyfinClient::items(const JfQuery& q, std::string& error)
{
    std::vector<JfItem> out;
    if (userId_.empty()) { error = "Not signed in"; return out; }

    std::string qs = "?userId=" + Http::UrlEncode(userId_);
    qs += "&Recursive=" + std::string(q.recursive ? "true" : "false");
    qs += "&SortBy=" + Http::UrlEncode(q.sortBy);
    qs += "&SortOrder=" + Http::UrlEncode(q.sortOrder);
    // ImageTags is what makes a cache-correct artwork URL possible.
    qs += "&Fields=Overview,ProductionYear,PrimaryImageAspectRatio";
    qs += "&EnableImageTypes=Primary";
    qs += "&EnableUserData=true";
    if (!q.parentId.empty())         qs += "&ParentId=" + Http::UrlEncode(q.parentId);
    if (!q.includeItemTypes.empty()) qs += "&IncludeItemTypes=" + Http::UrlEncode(q.includeItemTypes);
    if (!q.searchTerm.empty())       qs += "&SearchTerm=" + Http::UrlEncode(q.searchTerm);
    if (q.startIndex > 0)            qs += "&StartIndex=" + bj::ToString(q.startIndex);
    if (q.limit > 0)                 qs += "&Limit=" + bj::ToString(q.limit);

    const std::string body = getJson("/Items" + qs,
                                     "/Users/" + Http::UrlEncode(userId_) + "/Items" + qs,
                                     error);
    if (body.empty()) return out;

    JsonDoc doc(body);
    for (const JsonValue& obj : doc["Items"].items()) {
        JfItem it = ParseItem(obj);
        if (!it.id.empty()) out.push_back(it);
    }
    return out;
}

bool JellyfinClient::itemDetails(const std::string& itemId,
                                 JfItem& out,
                                 std::string& error)
{
    if (userId_.empty()) { error = "Not signed in"; return false; }

    const std::string body =
        getJson("/Items/" + Http::UrlEncode(itemId) + "?userId=" + Http::UrlEncode(userId_) +
                "&fields=Overview,Genres,Taglines,ProductionYear",
                "/Users/" + Http::UrlEncode(userId_) + "/Items/" + Http::UrlEncode(itemId),
                error);
    if (body.empty()) return false;

    JsonDoc doc(body);
    if (!doc.valid()) { error = "Unexpected reply"; return false; }
    out = ParseItem(doc.root());
    return !out.id.empty();
}

std::vector<JfItem> JellyfinClient::seasons(const std::string& seriesId,
                                            std::string& error)
{
    std::vector<JfItem> out;
    if (userId_.empty()) { error = "Not signed in"; return out; }

    const std::string body = getJson(
        "/Shows/" + Http::UrlEncode(seriesId) + "/Seasons"
        "?userId=" + Http::UrlEncode(userId_) +
        "&enableImageTypes=Primary", "", error);
    if (body.empty()) return out;

    JsonDoc doc(body);
    for (const JsonValue& obj : doc["Items"].items()) {
        JfItem it = ParseItem(obj);
        if (!it.id.empty()) out.push_back(it);
    }
    return out;
}

std::vector<JfItem> JellyfinClient::episodes(const std::string& seriesId,
                                             const std::string& seasonId,
                                             std::string& error)
{
    std::vector<JfItem> out;
    if (userId_.empty()) { error = "Not signed in"; return out; }

    std::string path = "/Shows/" + Http::UrlEncode(seriesId) + "/Episodes"
                       "?userId=" + Http::UrlEncode(userId_) +
                       "&fields=Overview"
                       "&enableImageTypes=Primary";
    if (!seasonId.empty()) path += "&seasonId=" + Http::UrlEncode(seasonId);

    const std::string body = getJson(path, "", error);
    if (body.empty()) return out;

    JsonDoc doc(body);
    for (const JsonValue& obj : doc["Items"].items()) {
        JfItem it = ParseItem(obj);
        if (!it.id.empty()) out.push_back(it);
    }
    return out;
}

// --------------------------------------------------------------------- URLs

std::string JellyfinClient::imageUrl(const std::string& itemId,
                                     const std::string& primaryTag,
                                     int maxWidth,
                                     int maxHeight) const
{
    if (itemId.empty() || serverUrl_.empty()) return "";
    // fillWidth with fillHeight asks the server to cover that box and crop the
    // overflow, which is the same thing drawTextureCover does but done once,
    // server side, with a proper resample rather than a bilinear tap.
    std::string u = serverUrl_ + "/Items/" + Http::UrlEncode(itemId) +
                    "/Images/Primary?fillWidth=" + bj::ToString(maxWidth) +
                    "&fillHeight=" + bj::ToString(maxHeight) +
                    "&quality=90";
    if (!primaryTag.empty()) u += "&tag=" + Http::UrlEncode(primaryTag);
    return u;
}

std::string JellyfinClient::audioPcmUrl(const std::string& itemId) const
{
    if (itemId.empty() || serverUrl_.empty() || accessToken_.empty()) return "";
    // The console has no audio decoder we can rely on, so the server hands
    // us finished PCM and we push the bytes straight at the mixer.
    return serverUrl_ + "/Audio/" + Http::UrlEncode(itemId) + "/universal"
           "?userId="   + Http::UrlEncode(userId_) +
           "&deviceId=" + Http::UrlEncode(deviceId_) +
           "&api_key="  + Http::UrlEncode(accessToken_) +
           "&audioCodec=pcm"
           "&container=wav"
           "&transcodingContainer=wav"
           "&transcodingProtocol=http"
           "&maxAudioChannels=2"
           "&maxStreamingBitrate=1536000";
}

std::string JellyfinClient::audioDirectUrl(const std::string& itemId) const
{
    if (itemId.empty() || serverUrl_.empty() || accessToken_.empty()) return "";
    return serverUrl_ + "/Audio/" + Http::UrlEncode(itemId) +
           "/stream?static=true&api_key=" + Http::UrlEncode(accessToken_);
}

// ------------------------------------------------------------ home rows

// Every row below returns the same shape, so the grid renders them without
// caring which one it is looking at.
std::vector<JfItem> JellyfinClient::parseItemList(const std::string& body)
{
    std::vector<JfItem> out;
    JsonDoc doc(body);
    for (const JsonValue& obj : doc["Items"].items()) {
        JfItem it = ParseItem(obj);
        if (!it.id.empty()) out.push_back(it);
    }
    return out;
}

std::vector<JfItem> JellyfinClient::resumable(std::string& error, int limit)
{
    if (userId_.empty()) { error = "Not signed in"; return {}; }
    const std::string body = getJson(
        "/UserItems/Resume?userId=" + Http::UrlEncode(userId_) +
        "&limit=" + bj::ToString(limit) +
        "&mediaTypes=Video&enableUserData=true&fields=Overview,ProductionYear"
        "&enableImageTypes=Primary",
        "/Users/" + Http::UrlEncode(userId_) + "/Items/Resume?limit=" +
        bj::ToString(limit) + "&mediaTypes=Video&enableUserData=true",
        error);
    return body.empty() ? std::vector<JfItem>() : parseItemList(body);
}

std::vector<JfItem> JellyfinClient::nextUp(std::string& error, int limit)
{
    if (userId_.empty()) { error = "Not signed in"; return {}; }
    const std::string body = getJson(
        "/Shows/NextUp?userId=" + Http::UrlEncode(userId_) +
        "&limit=" + bj::ToString(limit) +
        "&enableUserData=true&fields=Overview,ProductionYear"
        "&enableImageTypes=Primary", "", error);
    return body.empty() ? std::vector<JfItem>() : parseItemList(body);
}

std::vector<JfItem> JellyfinClient::recentlyAdded(std::string& error, int limit)
{
    if (userId_.empty()) { error = "Not signed in"; return {}; }
    const std::string body = getJson(
        "/Items/Latest?userId=" + Http::UrlEncode(userId_) +
        "&limit=" + bj::ToString(limit) +
        "&enableUserData=true&fields=Overview,ProductionYear"
        "&enableImageTypes=Primary", "", error);
    // Latest returns a bare array rather than an object with Items.
    if (body.empty()) return {};
    JsonDoc doc(body);
    std::vector<JfItem> out;
    for (const JsonValue& obj : doc.root().items()) {
        JfItem it = ParseItem(obj);
        if (!it.id.empty()) out.push_back(it);
    }
    if (out.empty()) out = parseItemList(body);
    return out;
}

std::vector<JfItem> JellyfinClient::search(const std::string& term,
                                           std::string& error, int limit)
{
    if (userId_.empty() || term.empty()) return {};
    JfQuery q;
    q.searchTerm = term;
    q.recursive  = true;
    q.limit      = limit;
    q.includeItemTypes = "Movie,Series,Episode";
    return items(q, error);
}

void JellyfinClient::markPlayed(const std::string& itemId, bool played)
{
    if (!signedIn() || itemId.empty()) return;
    const std::string url = apiUrl("/UserPlayedItems/" + Http::UrlEncode(itemId) +
                                   "?userId=" + Http::UrlEncode(userId_));
    if (played) Http::Post(url, "", HeadersFor(authHeaderValue()));
    else        Http::Delete(url, HeadersFor(authHeaderValue()));
}

// --------------------------------------------------------- video playback

bool JellyfinClient::playbackInfo(const std::string& itemId,
                                  PlaybackPlan& out, std::string& error)
{
    if (userId_.empty()) { error = "Not signed in"; return false; }

    const std::string body = getJson(
        "/Items/" + Http::UrlEncode(itemId) + "/PlaybackInfo"
        "?userId=" + Http::UrlEncode(userId_), "", error);
    if (body.empty()) return false;

    JsonDoc doc(body);
    out.playSessionId = doc["PlaySessionId"].asString();

    JsonValue sources = doc["MediaSources"];
    if (sources.size() == 0) { error = "The server offered no way to play this"; return false; }

    JsonValue source = sources.at(0);
    out.mediaSourceId = source.str("Id");
    out.directPlay    = source.flag("SupportsDirectPlay");

    // Copy the video stream's own parameters. Handing them back unchanged is
    // what lets the server remux rather than re-encode.
    for (const JsonValue& stream : source["MediaStreams"].items()) {
        if (stream.str("Type") != "Video") continue;
        out.videoCodec   = stream.str("Codec");
        out.videoProfile = stream.str("Profile");
        out.videoLevel   = stream.num("Level");
        out.width        = stream.num("Width");
        out.height       = stream.num("Height");

        // "16:9" or "1.85:1", either of which reduces to a number.
        const std::string aspect = stream.str("AspectRatio");
        const size_t colon = aspect.find(':');
        if (colon != std::string::npos) {
            const double left  = std::atof(aspect.substr(0, colon).c_str());
            const double right = std::atof(aspect.substr(colon + 1).c_str());
            if (left > 0.0 && right > 0.0) out.displayAspect = left / right;
        }
        break;
    }

    if (out.mediaSourceId.empty()) { error = "The server named no media source"; return false; }
    return true;
}

int JellyfinClient::VideoBitrateFor(int height)
{
    // Roughly what these sizes want for live action at h264. Generous rather
    // than tight: a wired LAN has the bandwidth, and starving the encoder
    // shows up as mush on a television.
    int bitrate = 8000000;
    if (height <= 360)      bitrate = 1500000;
    else if (height <= 480) bitrate = 2500000;
    else if (height <= 576) bitrate = 3500000;
    else if (height <= 720) bitrate = 5000000;

    // Except when the link cannot carry it. Asking for more than arrives is
    // worse than asking for less: the picture stays the same size either way,
    // and the alternative is the frame queue emptying every few seconds.
    // Resolution is never reduced here, only the bitrate at that resolution.
    const uint32_t ceiling = Platform::LinkBitrateCeiling();
    if (ceiling > 0 && bitrate > (int)ceiling) bitrate = (int)ceiling;

    return bitrate;
}

std::string JellyfinClient::videoHlsUrl(const PlaybackPlan& plan, int maxHeight,
                                        int64_t startTicks) const
{
    if (serverUrl_.empty() || accessToken_.empty()) return "";

    // Lower-case the profile: the server matches it case sensitively when
    // deciding whether the source stream can be copied.
    std::string profile = plan.videoProfile;
    for (char& ch : profile) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    if (profile.empty()) profile = "high";

    const int level = plan.videoLevel > 0 ? plan.videoLevel : 41;

    std::string url = serverUrl_ + "/Videos/" +
                      Http::UrlEncode(plan.mediaSourceId) + "/master.m3u8";
    url += "?mediaSourceId=" + Http::UrlEncode(plan.mediaSourceId);
    url += "&deviceId="      + Http::UrlEncode(deviceId_);
    url += "&api_key="       + Http::UrlEncode(accessToken_);
    url += "&playSessionId=" + Http::UrlEncode(plan.playSessionId);
    url += "&videoCodec=h264";
    url += "&profile="       + Http::UrlEncode(profile);
    url += "&level="         + bj::ToString(level);
    url += "&maxHeight="     + bj::ToString(maxHeight);
    url += "&videoBitRate="  + bj::ToString(VideoBitrateFor(maxHeight));
    // MP3 rather than AAC: the console has an MP3 decoder available and no
    // AAC one, and the bitrate difference does not matter over a LAN.
    url += "&audioCodec=mp3";
    url += "&maxAudioChannels=2";
    url += "&audioBitRate=192000";
    url += "&segmentContainer=ts";
    url += "&requireAvc=true";
    // Transcode rather than copy.
    //
    // Left to itself the server sees H.264 in, H.264 out and copies the
    // stream untouched, which ignores maxHeight, so asking for 360p and
    // getting the source's 720x480 is exactly what happened on the Xbox 360.
    // A console decoding in software needs the smaller picture more than it
    // needs the server's spare cycles.
    url += "&allowVideoStreamCopy=false";
    // And the switch that actually decides it: Jellyfin picks stream copy on
    // its own when the input already matches, and allowVideoStreamCopy alone
    // did not stop it, and the console still received the source's full size.
    url += "&enableAutoStreamCopy=false";
    url += "&breakOnNonKeyFrames=false";
    (void)startTicks;   // seeking picks a segment instead; see Player
    return url;
}

void JellyfinClient::stopTranscode(const std::string& playSessionId)
{
    if (!signedIn() || playSessionId.empty()) return;

    // There is no DELETE helper in the HTTP layer, and adding one for a
    // single fire-and-forget call is not worth it; the server also drops the
    // transcode itself once nothing fetches segments for a while.
    const std::string url = apiUrl("/Videos/ActiveEncodings"
                                   "?deviceId=" + Http::UrlEncode(deviceId_) +
                                   "&playSessionId=" + Http::UrlEncode(playSessionId));
    Http::Delete(url, HeadersFor(authHeaderValue()));
}

// -------------------------------------------------------- playback reporting

void JellyfinClient::reportPlaybackStart(const std::string& itemId)
{
    if (!signedIn()) return;
    const std::string body =
        "{\"ItemId\":\"" + JsonEscape(itemId) + "\","
        "\"PositionTicks\":0,\"IsPaused\":false,\"PlayMethod\":\"Transcode\"}";
    Http::Post(apiUrl("/Sessions/Playing"), body, HeadersFor(authHeaderValue()));
}

void JellyfinClient::reportPlaybackProgress(const std::string& itemId,
                                            int64_t positionTicks,
                                            bool paused)
{
    if (!signedIn()) return;
    char ticks[32];
    const std::string ticksText = bj::ToString((long long)positionTicks);
    std::snprintf(ticks, sizeof(ticks), "%s", ticksText.c_str());
    const std::string body =
        "{\"ItemId\":\"" + JsonEscape(itemId) + "\","
        "\"PositionTicks\":" + ticks + ","
        "\"IsPaused\":" + (paused ? "true" : "false") + ","
        "\"PlayMethod\":\"Transcode\"}";
    Http::Post(apiUrl("/Sessions/Playing/Progress"), body, HeadersFor(authHeaderValue()));
}

void JellyfinClient::reportPlaybackStopped(const std::string& itemId,
                                           int64_t positionTicks)
{
    if (!signedIn()) return;
    char ticks[32];
    const std::string ticksText = bj::ToString((long long)positionTicks);
    std::snprintf(ticks, sizeof(ticks), "%s", ticksText.c_str());
    const std::string body =
        "{\"ItemId\":\"" + JsonEscape(itemId) + "\","
        "\"PositionTicks\":" + ticks + "}";
    Http::Post(apiUrl("/Sessions/Playing/Stopped"), body, HeadersFor(authHeaderValue()));
}
