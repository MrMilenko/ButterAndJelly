// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/jellyfin.h"

#include "core/features.h"
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

// One file per server, named for the URL with anything awkward flattened.
std::string SessionPathFor(const std::string& serverUrl)
{
    std::string key;
    key.reserve(serverUrl.size());
    for (char ch : serverUrl) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9');
        key += safe ? ch : '_';
    }
    if (key.empty()) key = "server";
    return Platform::DataDir() + "/sessions_" + key + ".json";
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

// Genres, Studios and the like arrive as arrays of strings or of objects
// with a Name. Both reduce to one comma separated line.
std::string JoinNames(const JsonValue& array)
{
    std::string joined;
    for (const JsonValue& entry : array.items()) {
        std::string name = entry.asString();
        if (name.empty()) name = entry.str("Name");
        if (name.empty()) continue;
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

JfStream ParseStream(const JsonValue& obj)
{
    JfStream st;
    st.index    = obj.num("Index");
    st.type     = obj.str("Type");
    st.codec    = obj.str("Codec");
    st.language = obj.str("Language");
    st.title    = obj.str("DisplayTitle");
    if (st.title.empty()) st.title = obj.str("Title");
    st.deliveryUrl = obj.str("DeliveryUrl");
    st.channels    = obj.num("Channels");
    st.isDefault   = obj.flag("IsDefault");
    st.isForced    = obj.flag("IsForced");
    st.isExternal  = obj.flag("IsExternal");
    st.isText      = obj.flag("IsTextSubtitleStream");
    st.isHearingImpaired = obj.flag("IsHearingImpaired");
    return st;
}

JfMediaSource ParseMediaSource(const JsonValue& obj)
{
    JfMediaSource src;
    src.id        = obj.str("Id");
    src.name      = obj.str("Name");
    src.container = obj.str("Container");
    src.size         = (int64_t)obj["Size"].asDouble(0.0);
    src.runTimeTicks = (int64_t)obj["RunTimeTicks"].asDouble(0.0);
    src.bitrate      = obj.num("Bitrate");
    src.supportsDirectPlay   = obj.flag("SupportsDirectPlay");
    src.supportsDirectStream = obj.flag("SupportsDirectStream");
    src.supportsTranscoding  = obj.flag("SupportsTranscoding");

    JsonValue audioDefault = obj["DefaultAudioStreamIndex"];
    src.defaultAudioIndex = audioDefault.valid() ? audioDefault.asInt(-1) : -1;
    JsonValue subDefault = obj["DefaultSubtitleStreamIndex"];
    src.defaultSubtitleIndex = subDefault.valid() ? subDefault.asInt(-1) : -1;

    for (const JsonValue& stream : obj["MediaStreams"].items()) {
        src.streams.push_back(ParseStream(stream));
    }
    return src;
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
    it.criticRating    = obj["CriticRating"].asDouble(0.0);
    it.tagline         = obj["Taglines"].size() > 0
                       ? obj["Taglines"].at(0).asString() : "";
    it.premiereDate    = obj.str("PremiereDate");
    it.status          = obj.str("Status");
    it.childCount      = obj.num("ChildCount");
    it.mediaSourceCount = obj.num("MediaSourceCount");
    it.genres          = JoinNames(obj["Genres"]);
    it.studios         = JoinNames(obj["Studios"]);

    for (const JsonValue& chapter : obj["Chapters"].items()) {
        JfChapter ch;
        ch.startTicks = (int64_t)chapter["StartPositionTicks"].asDouble(0.0);
        ch.name       = chapter.str("Name");
        ch.imageTag   = chapter.str("ImageTag");
        it.chapters.push_back(ch);
    }

    for (const JsonValue& person : obj["People"].items()) {
        JfPerson pn;
        pn.id         = person.str("Id");
        pn.name       = person.str("Name");
        pn.role       = person.str("Role");
        pn.type       = person.str("Type");
        pn.primaryTag = person.str("PrimaryImageTag");
        if (!pn.name.empty()) it.people.push_back(pn);
    }

    JsonValue userData = obj["UserData"];
    if (userData.valid()) {
        it.resumeTicks    = (int64_t)userData["PlaybackPositionTicks"].asDouble(0.0);
        it.played         = userData.flag("Played");
        it.isFavorite     = userData.flag("IsFavorite");
        it.playedFraction = userData["PlayedPercentage"].asDouble(0.0) / 100.0;
        if (it.playedFraction <= 0.0 && it.runTimeTicks > 0 && it.resumeTicks > 0) {
            it.playedFraction = (double)it.resumeTicks / (double)it.runTimeTicks;
        }
    }
    return it;
}

// "eng" is not something to put in front of a user.
struct LanguageName { const char* code; const char* name; };
const LanguageName kLanguages[] = {
    { "eng", "English" },   { "spa", "Spanish" },   { "fre", "French" },
    { "fra", "French" },    { "ger", "German" },    { "deu", "German" },
    { "ita", "Italian" },   { "por", "Portuguese" },{ "rus", "Russian" },
    { "jpn", "Japanese" },  { "kor", "Korean" },    { "chi", "Chinese" },
    { "zho", "Chinese" },   { "dut", "Dutch" },     { "nld", "Dutch" },
    { "swe", "Swedish" },   { "nor", "Norwegian" }, { "dan", "Danish" },
    { "fin", "Finnish" },   { "pol", "Polish" },    { "tur", "Turkish" },
    { "ara", "Arabic" },    { "heb", "Hebrew" },    { "hin", "Hindi" },
    { "tha", "Thai" },      { "vie", "Vietnamese" },{ "ces", "Czech" },
    { "cze", "Czech" },     { "hun", "Hungarian" }, { "ell", "Greek" },
    { "gre", "Greek" },     { "ukr", "Ukrainian" },
};

std::string LanguageLabel(const std::string& code)
{
    for (const LanguageName& lang : kLanguages) {
        if (code == lang.code) return lang.name;
    }
    return code;
}

}  // namespace

std::string JfStream::label() const
{
    if (!title.empty()) return title;

    std::string out = language.empty() ? std::string("Unknown")
                                       : LanguageLabel(language);
    if (isForced) out += " (Forced)";
    if (isHearingImpaired) out += " (SDH)";
    if (channels == 2)     out += " Stereo";
    else if (channels > 2) out += " " + bj::ToString(channels) + ".0";
    if (!codec.empty()) {
        std::string upper = codec;
        for (char& ch : upper) {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        }
        out += " " + upper;
    }
    return out;
}

const JfStream* JfMediaSource::streamAt(int index) const
{
    for (const JfStream& stream : streams) {
        if (stream.index == index) return &stream;
    }
    return nullptr;
}

std::vector<const JfStream*> JfMediaSource::ofType(const char* type) const
{
    std::vector<const JfStream*> out;
    for (const JfStream& stream : streams) {
        if (stream.type == type) out.push_back(&stream);
    }
    return out;
}

const JfMediaSource* JellyfinClient::PlaybackPlan::source() const
{
    if (sourceIndex < 0 || sourceIndex >= (int)sources.size()) return nullptr;
    return &sources[(size_t)sourceIndex];
}

const JfSortOption* JfSortOptions(int& count)
{
    static const JfSortOption kOptions[] = {
        { "Name",           "SortName",                        false },
        { "Date added",     "DateCreated,SortName",            true  },
        { "Release date",   "PremiereDate,ProductionYear,SortName", true },
        { "Rating",         "CommunityRating,SortName",        true  },
        { "Critic rating",  "CriticRating,SortName",           true  },
        { "Runtime",        "Runtime,SortName",                false },
        { "Play count",     "PlayCount,SortName",              true  },
        { "Last played",    "DatePlayed,SortName",             true  },
        { "Random",         "Random",                          false },
    };
    count = (int)(sizeof(kOptions) / sizeof(kOptions[0]));
    return kOptions;
}

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

bool JellyfinClient::authorizeQuickConnect(const std::string& code,
                                           std::string& error)
{
    if (!signedIn()) { error = "Not signed in"; return false; }
    if (code.empty()) { error = "No code"; return false; }

    HttpResponse r = Http::Post(
        apiUrl("/QuickConnect/Authorize?code=" + Http::UrlEncode(code) +
               "&userId=" + Http::UrlEncode(userId_)),
        "", HeadersFor(authHeaderValue()));
    if (!r.ok()) {
        error = r.status == 403 ? "This account may not approve codes"
                                : "The server would not approve that code";
        return false;
    }
    return true;
}

std::string JellyfinClient::serverHost() const
{
    std::string host = serverUrl_;
    const size_t scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    const size_t colon = host.rfind(':');
    // Leaves an IPv6 literal alone: it has colons of its own.
    if (colon != std::string::npos && host.find(']') == std::string::npos) {
        host = host.substr(0, colon);
    }
    return host;
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
    // Once under a fixed name for the last session, once under the server's
    // own so coming back to it needs no Quick Connect.
    const std::string paths[] = { SessionPath(), SessionPathFor(serverUrl_) };
    bool wroteAny = false;
    for (const std::string& path : paths) {
        FILE* fp = std::fopen(Platform::NativePath(path).c_str(), "wb");
        if (!fp) continue;
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
        wroteAny = true;
    }
    return wroteAny;
}

bool JellyfinClient::loadSessionFor(const std::string& serverUrl)
{
    const std::string text = ReadWholeFile(SessionPathFor(serverUrl));
    if (text.empty()) return false;

    JsonDoc doc(text);
    if (!doc.valid()) return false;

    const std::string token = doc["accessToken"].asString();
    if (token.empty()) return false;

    serverUrl_   = doc["serverUrl"].asString();
    accessToken_ = token;
    userId_      = doc["userId"].asString();
    userName_    = doc["userName"].asString();
    const std::string storedDevice = doc["deviceId"].asString();
    if (!storedDevice.empty()) deviceId_ = storedDevice;

    saveSession();   // make it the active one again
    return !serverUrl_.empty();
}

// Drops the session from memory, keeping the copy on disk.
void JellyfinClient::releaseSession()
{
    accessToken_.clear();
    userId_.clear();
    userName_.clear();
    qcSecret_.clear();
    qcCode_.clear();
    std::remove(Platform::NativePath(SessionPath()).c_str());
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
    if (!serverUrl_.empty()) {
        std::remove(Platform::NativePath(SessionPathFor(serverUrl_)).c_str());
    }
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
    int ignored = 0;
    return items(q, error, ignored);
}

std::vector<JfItem> JellyfinClient::items(const JfQuery& q, std::string& error,
                                          int& totalCount)
{
    std::vector<JfItem> out;
    if (userId_.empty()) { error = "Not signed in"; return out; }

    std::string qs = "?userId=" + Http::UrlEncode(userId_);
    qs += "&Recursive=" + std::string(q.recursive ? "true" : "false");
    qs += "&SortBy=" + Http::UrlEncode(q.sortBy);
    qs += "&SortOrder=" + Http::UrlEncode(q.sortOrder);
    // ImageTags is what makes a cache-correct artwork URL possible.
    qs += "&Fields=Overview,ProductionYear,PrimaryImageAspectRatio";
    if (!q.extraFields.empty()) qs += "," + Http::UrlEncode(q.extraFields);
    qs += "&EnableImageTypes=Primary";
    qs += "&EnableUserData=true";
    if (!q.parentId.empty())         qs += "&ParentId=" + Http::UrlEncode(q.parentId);
    if (!q.includeItemTypes.empty()) qs += "&IncludeItemTypes=" + Http::UrlEncode(q.includeItemTypes);
    if (!q.searchTerm.empty())       qs += "&SearchTerm=" + Http::UrlEncode(q.searchTerm);
    if (q.startIndex > 0)            qs += "&StartIndex=" + bj::ToString(q.startIndex);
    if (q.limit > 0)                 qs += "&Limit=" + bj::ToString(q.limit);
    if (!q.filters.empty())          qs += "&Filters=" + Http::UrlEncode(q.filters);
    if (!q.genres.empty())           qs += "&Genres=" + Http::UrlEncode(q.genres);
    if (!q.years.empty())            qs += "&Years=" + Http::UrlEncode(q.years);
    if (!q.officialRatings.empty())  qs += "&OfficialRatings=" + Http::UrlEncode(q.officialRatings);
    if (!q.studios.empty())          qs += "&Studios=" + Http::UrlEncode(q.studios);
    if (!q.nameStartsWith.empty())   qs += "&NameStartsWith=" + Http::UrlEncode(q.nameStartsWith);
    if (!q.mediaTypes.empty())       qs += "&MediaTypes=" + Http::UrlEncode(q.mediaTypes);
    if (q.isFavorite >= 0) qs += std::string("&IsFavorite=") + (q.isFavorite ? "true" : "false");
    if (q.isPlayed   >= 0) qs += std::string("&IsPlayed=")   + (q.isPlayed   ? "true" : "false");

    const std::string body = getJson("/Items" + qs,
                                     "/Users/" + Http::UrlEncode(userId_) + "/Items" + qs,
                                     error);
    if (body.empty()) return out;

    JsonDoc doc(body);
    totalCount = doc["TotalRecordCount"].asInt(0);
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

bool JellyfinClient::itemDetailsFull(const std::string& itemId,
                                    JfItem& out,
                                    std::string& error)
{
    if (userId_.empty()) { error = "Not signed in"; return false; }

    const std::string fields =
        "Overview,Genres,Studios,Taglines,ProductionYear,Chapters,People,"
        "MediaSourceCount,ChildCount,ExternalUrls,RemoteTrailers";
    const std::string body =
        getJson("/Items/" + Http::UrlEncode(itemId) + "?userId=" + Http::UrlEncode(userId_) +
                "&fields=" + Http::UrlEncode(fields),
                "/Users/" + Http::UrlEncode(userId_) + "/Items/" + Http::UrlEncode(itemId) +
                "?fields=" + Http::UrlEncode(fields),
                error);
    if (body.empty()) return false;

    JsonDoc doc(body);
    if (!doc.valid()) { error = "Unexpected reply"; return false; }
    out = ParseItem(doc.root());
    return !out.id.empty();
}

bool JellyfinClient::filterOptions(const std::string& parentId,
                                   JfFilterOptions& out,
                                   std::string& error)
{
    if (userId_.empty()) { error = "Not signed in"; return false; }

    std::string path = "/Items/Filters2?userId=" + Http::UrlEncode(userId_);
    if (!parentId.empty()) path += "&parentId=" + Http::UrlEncode(parentId);

    const std::string body = getJson(path, "", error);
    if (body.empty()) return false;

    JsonDoc doc(body);
    if (!doc.valid()) { error = "Unexpected reply"; return false; }

    // Genres come back as objects with a Name; the rest are plain strings.
    for (const JsonValue& genre : doc["Genres"].items()) {
        std::string name = genre.str("Name");
        if (name.empty()) name = genre.asString();
        if (!name.empty()) out.genres.push_back(name);
    }
    for (const JsonValue& tag : doc["Tags"].items()) {
        const std::string name = tag.asString();
        if (!name.empty()) out.tags.push_back(name);
    }
    return true;
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
    // fillWidth with fillHeight covers the box and crops the overflow, server
    // side, with a proper resample.
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

std::vector<JfItem> JellyfinClient::favorites(std::string& error, int limit)
{
    if (userId_.empty()) { error = "Not signed in"; return {}; }

    JfQuery q;
    q.recursive        = true;
    q.isFavorite       = 1;
    q.includeItemTypes = "Movie,Series,Episode,MusicAlbum";
    q.sortBy           = "SortName";
    q.limit            = limit;
    return items(q, error);
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

void JellyfinClient::setFavorite(const std::string& itemId, bool favorite)
{
    if (!signedIn() || itemId.empty()) return;
    const std::string url = apiUrl("/UserFavoriteItems/" + Http::UrlEncode(itemId) +
                                   "?userId=" + Http::UrlEncode(userId_));
    if (favorite) Http::Post(url, "", HeadersFor(authHeaderValue()));
    else          Http::Delete(url, HeadersFor(authHeaderValue()));
}

// --------------------------------------------------------- video playback

// What this build can play. Without it PlaybackInfo only describes the file.
static std::string DeviceProfileJson()
{
    std::string json = "{";
    json += "\"Name\":\"" + std::string(kAppName) + "\",";
    json += "\"MaxStreamingBitrate\":" +
            bj::ToString(JellyfinClient::VideoBitrateFor(BJ_MAX_VIDEO_HEIGHT)) + ",";

    json += "\"DirectPlayProfiles\":[{"
            "\"Container\":\"" BJ_DIRECT_PLAY_CONTAINERS "\","
            "\"Type\":\"Video\","
            "\"VideoCodec\":\"" BJ_VIDEO_CODECS "\","
            "\"AudioCodec\":\"" BJ_AUDIO_CODECS "\"},"
            "{\"Container\":\"mp3\",\"Type\":\"Audio\"}],";

    json += "\"TranscodingProfiles\":[{"
            "\"Container\":\"ts\","
            "\"Type\":\"Video\","
            "\"VideoCodec\":\"" BJ_VIDEO_CODECS "\","
            "\"AudioCodec\":\"" BJ_AUDIO_CODECS "\","
            "\"Protocol\":\"hls\","
            "\"Context\":\"Streaming\","
            "\"MaxAudioChannels\":\"2\","
            "\"MinSegments\":1,"
            "\"BreakOnNonKeyFrames\":true}],";

    // External hands text tracks over as a file; the rest are burned in.
    json += "\"SubtitleProfiles\":[";
    const char* formats[] = { "vtt", "srt", "subrip", "ass", "ssa" };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        if (i) json += ",";
        json += "{\"Format\":\"" + std::string(formats[i]) +
                "\",\"Method\":\"External\"}";
    }
    json += ",{\"Format\":\"pgssub\",\"Method\":\"Encode\"},"
            "{\"Format\":\"dvdsub\",\"Method\":\"Encode\"}],";

    json += "\"CodecProfiles\":[],\"ContainerProfiles\":[]}";
    return json;
}

bool JellyfinClient::playbackInfo(const std::string& itemId,
                                  PlaybackPlan& out, std::string& error)
{
    return playbackInfo(itemId, PlaybackRequest(), out, error);
}

bool JellyfinClient::playbackInfo(const std::string& itemId,
                                  const PlaybackRequest& request,
                                  PlaybackPlan& out, std::string& error)
{
    if (userId_.empty()) { error = "Not signed in"; return false; }

    std::string path = "/Items/" + Http::UrlEncode(itemId) + "/PlaybackInfo"
                       "?userId=" + Http::UrlEncode(userId_);
    if (!request.mediaSourceId.empty()) {
        path += "&mediaSourceId=" + Http::UrlEncode(request.mediaSourceId);
    }
    if (request.audioIndex >= 0) {
        path += "&audioStreamIndex=" + bj::ToString(request.audioIndex);
    }
    if (request.subtitleIndex >= 0) {
        path += "&subtitleStreamIndex=" + bj::ToString(request.subtitleIndex);
    }
    if (request.startTicks > 0) {
        path += "&startTimeTicks=" + bj::ToString((long long)request.startTicks);
    }
    const int bitrate = request.maxBitrate > 0
                      ? request.maxBitrate : VideoBitrateFor(BJ_MAX_VIDEO_HEIGHT);
    path += "&maxStreamingBitrate=" + bj::ToString(bitrate);
    path += "&maxAudioChannels=2";

    const std::string requestBody =
        "{\"DeviceProfile\":" + DeviceProfileJson() + "}";

    HttpResponse response = Http::Post(apiUrl(path), requestBody,
                                       HeadersFor(authHeaderValue()));
    // Older servers took this as a GET with no body at all.
    std::string body = response.ok() ? response.body : std::string();
    if (body.empty()) {
        body = getJson(path, "", error);
        if (body.empty()) return false;
    }

    JsonDoc doc(body);
    out.playSessionId = doc["PlaySessionId"].asString();
    out.sources.clear();

    for (const JsonValue& source : doc["MediaSources"].items()) {
        out.sources.push_back(ParseMediaSource(source));
    }
    if (out.sources.empty()) {
        error = "The server offered no way to play this";
        return false;
    }

    // Naming a version returns only that one, so this indexes what came back.
    out.sourceIndex = 0;
    for (size_t i = 0; i < out.sources.size(); ++i) {
        if (out.sources[i].id == request.mediaSourceId) {
            out.sourceIndex = (int)i;
            break;
        }
    }

    const JfMediaSource& chosen = out.sources[(size_t)out.sourceIndex];
    out.mediaSourceId = chosen.id;
    out.container     = chosen.container;
    out.directPlay    = chosen.supportsDirectPlay;
    out.audioIndex    = request.audioIndex >= 0 ? request.audioIndex
                                                : chosen.defaultAudioIndex;
    out.subtitleIndex = request.subtitleIndex == -2 ? -1
                      : (request.subtitleIndex >= 0 ? request.subtitleIndex
                                                    : chosen.defaultSubtitleIndex);

    // Copy the video stream's own parameters. Handing them back unchanged is
    // what lets the server remux rather than re-encode.
    for (const JsonValue& source : doc["MediaSources"].items()) {
        if (source.str("Id") != out.mediaSourceId) continue;
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
        break;
    }

    if (out.mediaSourceId.empty()) { error = "The server named no media source"; return false; }
    return true;
}

int JellyfinClient::VideoBitrateFor(int height)
{
    // Roughly what these sizes want for live action at h264, generous rather
    // than tight.
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

constexpr int kMaxVideoWidth     = BJ_MAX_VIDEO_WIDTH;
constexpr int kMaxVideoFramerate = BJ_MAX_FRAMERATE;

// Baseline is cheaper to decode, but asking for it pushed the server off its
// hardware encoder, which cost more than it saved.
constexpr const char* kForcedVideoProfile = nullptr;
constexpr int         kForcedVideoLevel   = 0;

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

    int level = plan.videoLevel > 0 ? plan.videoLevel : 41;

    // Following the source only helps while a stream copy is possible.
    if (kForcedVideoProfile) {
        profile = kForcedVideoProfile;
        level   = kForcedVideoLevel;
    }

    std::string url = serverUrl_ + "/Videos/" +
                      Http::UrlEncode(plan.mediaSourceId) + "/master.m3u8";
    url += "?mediaSourceId=" + Http::UrlEncode(plan.mediaSourceId);
    url += "&deviceId="      + Http::UrlEncode(deviceId_);
    url += "&api_key="       + Http::UrlEncode(accessToken_);
    url += "&playSessionId=" + Http::UrlEncode(plan.playSessionId);
    // H.264 everywhere: MPEG-4 is cheaper to decode, but the server has no
    // hardware encoder for it and the encode cost outweighs the saving.
#if defined(BJ_VIDEO_CODEC_MPEG4)
    // profile and level are H.264 spellings; the encoder rejects them here.
    url += "&videoCodec=mpeg4";
    (void)profile;
    (void)level;
#else
    url += "&videoCodec=h264";
    url += "&profile="       + Http::UrlEncode(profile);
    url += "&level="         + bj::ToString(level);
#endif
    // maxHeight 0 is the caller asking for the source untouched, which only a
    // machine with no decode ceiling should do.
    const bool capped = maxHeight > 0;
    if (capped) {
        url += "&maxHeight=" + bj::ToString(maxHeight);
        // A bitrate the viewer chose wins over the one the height implies.
        const int bitrate = prefs_.videoBitrate > 0 ? prefs_.videoBitrate * 1000
                                                    : VideoBitrateFor(maxHeight);
        url += "&videoBitRate=" + bj::ToString(bitrate);
    }
    if (kMaxVideoWidth > 0) {
        url += "&maxWidth=" + bj::ToString(kMaxVideoWidth);
    }
    // The platform ceiling stands whatever was asked for.
    int framerate = prefs_.maxFramerate;
    if (kMaxVideoFramerate > 0 &&
        (framerate == 0 || framerate > kMaxVideoFramerate)) {
        framerate = kMaxVideoFramerate;
    }
    if (framerate > 0) {
        url += "&maxFramerate=" + bj::ToString(framerate);
    }
    // Whatever this build can decode, best first. Asking for a codec the
    // source already uses is what lets the server skip the audio re-encode.
    url += "&audioCodec=" + (prefs_.audioCodecs.empty() ? std::string(kAudioCodecs)
                                                        : prefs_.audioCodecs);
    url += "&maxAudioChannels=2";
    if (capped) {
        const int audio = prefs_.audioBitrate > 0 ? prefs_.audioBitrate : 192;
        url += "&audioBitRate=" + bj::ToString(audio * 1000);
    }
    url += "&allowAudioStreamCopy=" + std::string(capped ? "false" : "true");
    url += "&segmentContainer=ts";
#if !defined(BJ_VIDEO_CODEC_MPEG4)
    // Asking for AVC specifically would undo the codec choice above.
    url += "&requireAvc=true";
#endif
    // A cap is ignored unless stream copy is refused, and uncapped a copy is
    // exactly what is wanted.
    if (capped) {
        url += "&allowVideoStreamCopy=false";
        url += "&enableAutoStreamCopy=false";
    } else {
        url += "&allowVideoStreamCopy=true";
        url += "&enableAutoStreamCopy=true";
    }
    url += "&breakOnNonKeyFrames=false";

    // A subtitle index only reaches here for a bitmap track, which has to be
    // burned into the picture.
    if (plan.audioIndex >= 0) {
        url += "&audioStreamIndex=" + bj::ToString(plan.audioIndex);
    }
    if (plan.subtitleIndex >= 0) {
        url += "&subtitleStreamIndex=" + bj::ToString(plan.subtitleIndex);
        url += "&subtitleMethod=Encode";
    }
    (void)startTicks;   // seeking picks a segment instead; see Player
    return url;
}

std::string JellyfinClient::videoDirectUrl(const PlaybackPlan& plan) const
{
    if (serverUrl_.empty() || accessToken_.empty()) return "";
    if (plan.mediaSourceId.empty()) return "";

    const std::string container = plan.container.empty() ? "ts" : plan.container;
    std::string url = serverUrl_ + "/Videos/" +
                      Http::UrlEncode(plan.mediaSourceId) + "/stream." + container;
    url += "?static=true";
    url += "&mediaSourceId=" + Http::UrlEncode(plan.mediaSourceId);
    url += "&deviceId="      + Http::UrlEncode(deviceId_);
    url += "&api_key="       + Http::UrlEncode(accessToken_);
    if (!plan.playSessionId.empty()) {
        url += "&playSessionId=" + Http::UrlEncode(plan.playSessionId);
    }
    return url;
}

std::string JellyfinClient::subtitleUrl(const std::string& itemId,
                                        const std::string& mediaSourceId,
                                        int streamIndex,
                                        const char* format) const
{
    if (serverUrl_.empty() || accessToken_.empty()) return "";
    if (itemId.empty() || streamIndex < 0) return "";

    std::string url = serverUrl_ + "/Videos/" + Http::UrlEncode(itemId) + "/" +
                      Http::UrlEncode(mediaSourceId) + "/Subtitles/" +
                      bj::ToString(streamIndex) + "/Stream." + format;
    url += "?api_key=" + Http::UrlEncode(accessToken_);
    return url;
}

void JellyfinClient::stopTranscode(const std::string& playSessionId)
{
    if (!signedIn() || playSessionId.empty()) return;

    // Best effort: the server drops the transcode itself once nothing has
    // fetched a segment for a while.
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
    const std::string body =
        "{\"ItemId\":\"" + JsonEscape(itemId) + "\","
        "\"PositionTicks\":" + bj::ToString((long long)positionTicks) + ","
        "\"IsPaused\":" + (paused ? "true" : "false") + ","
        "\"PlayMethod\":\"Transcode\"}";
    Http::Post(apiUrl("/Sessions/Playing/Progress"), body, HeadersFor(authHeaderValue()));
}

void JellyfinClient::reportPlaybackPing(const std::string& playSessionId)
{
    if (!signedIn() || playSessionId.empty()) return;
    Http::Post(apiUrl("/Sessions/Playing/Ping?playSessionId=" +
                      Http::UrlEncode(playSessionId)),
               "", HeadersFor(authHeaderValue()));
}

void JellyfinClient::reportPlaybackStopped(const std::string& itemId,
                                           int64_t positionTicks)
{
    if (!signedIn()) return;
    const std::string body =
        "{\"ItemId\":\"" + JsonEscape(itemId) + "\","
        "\"PositionTicks\":" + bj::ToString((long long)positionTicks) + "}";
    Http::Post(apiUrl("/Sessions/Playing/Stopped"), body, HeadersFor(authHeaderValue()));
}

// ---------------------------------------------------------------- subtitles

namespace {

// "00:01:02.500" or "01:02.500", either side of a WebVTT cue timing.
double ParseCueTime(const std::string& text)
{
    double parts[3] = { 0.0, 0.0, 0.0 };
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= text.size() && count < 3; ++i) {
        if (i == text.size() || text[i] == ':') {
            parts[count++] = std::atof(text.substr(start, i - start).c_str());
            start = i + 1;
        }
    }
    if (count == 3) return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
    if (count == 2) return parts[0] * 60.0 + parts[1];
    return parts[0];
}

// Drops <i>, {\an8} and the rest, which no font here can honor anyway.
std::string StripCueMarkup(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '<' || ch == '{') { ++depth; continue; }
        if (ch == '>' || ch == '}') { if (depth > 0) --depth; continue; }
        if (depth == 0) out += ch;
    }
    return out;
}

std::string TrimLine(const std::string& text)
{
    size_t begin = 0, end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r')) --end;
    return text.substr(begin, end - begin);
}

}  // namespace

std::vector<JellyfinClient::JfCue> JellyfinClient::subtitles(
    const std::string& itemId, const std::string& mediaSourceId,
    int streamIndex, std::string& error) const
{
    std::vector<JfCue> out;
    const std::string url = subtitleUrl(itemId, mediaSourceId, streamIndex, "vtt");
    if (url.empty()) { error = "Not signed in"; return out; }

    // Not HeadersFor: that asks for JSON, and this endpoint answers text.
    HttpHeaders headers;
    headers.push_back({ "Accept", "text/vtt, text/plain, */*" });
    headers.push_back({ "X-Emby-Authorization", authHeaderValue() });
    headers.push_back({ "Authorization", authHeaderValue() });

    HttpResponse response = Http::Get(url, headers);
    if (!response.ok()) {
        // The status alone: the URL carries the session token.
        error = "The server would not send that subtitle track (" +
                bj::ToString((long long)response.status) + ")";
        return out;
    }

    const std::string& body = response.body;
    JfCue cue;
    bool inCue = false;

    size_t pos = 0;
    while (pos <= body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        const std::string line = TrimLine(body.substr(pos, eol - pos));
        pos = eol + 1;

        const size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            if (inCue && !cue.text.empty()) out.push_back(cue);
            cue = JfCue();
            cue.start = ParseCueTime(TrimLine(line.substr(0, arrow)));
            // Anything after the end time is positioning, which is ignored.
            std::string rest = TrimLine(line.substr(arrow + 3));
            const size_t space = rest.find(' ');
            if (space != std::string::npos) rest = rest.substr(0, space);
            cue.end = ParseCueTime(rest);
            inCue = true;
            continue;
        }

        if (!inCue) continue;
        if (line.empty()) {
            if (!cue.text.empty()) out.push_back(cue);
            cue = JfCue();
            inCue = false;
            continue;
        }
        const std::string stripped = StripCueMarkup(line);
        if (stripped.empty()) continue;
        if (!cue.text.empty()) cue.text += "\n";
        cue.text += stripped;
    }
    if (inCue && !cue.text.empty()) out.push_back(cue);

    if (out.empty()) error = "That subtitle track was empty";
    return out;
}
