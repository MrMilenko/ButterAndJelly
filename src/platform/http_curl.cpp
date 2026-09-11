// SPDX-License-Identifier: GPL-2.0-or-later

// http_curl.cpp: libcurl implementation of core/http.h.
//
// Shared by both targets. The only divergence is Init(): on the console the
// network stack has to be brought up through nn::ac before any socket call
// will succeed, and there is no system CA store, so we point curl at a
// bundle shipped inside the .wuhb.

#include "core/http.h"
#include "core/log.h"
#include "core/platform.h"

#include <curl/curl.h>

#include <cstdio>
#include <atomic>
#include <cstring>
#include <mutex>

#ifdef __WIIU__
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <nn/ac.h>
#include <nn/result.h>
#include <sys/socket.h>
#endif

namespace {

bool  g_initialized = false;
std::atomic<bool> g_abort{false};
long  g_timeoutSeconds = 30;
std::string g_caBundlePath;   // empty => rely on the system store

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* fp = static_cast<FILE*>(userdata);
    return fwrite(ptr, size, nmemb, fp) * size;
}

struct SinkCtx {
    const HttpSink* sink;
    bool aborted = false;
};

size_t WriteToSink(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* ctx = static_cast<SinkCtx*>(userdata);
    const size_t total = size * nmemb;
    if (!(*ctx->sink)(reinterpret_cast<const uint8_t*>(ptr), total)) {
        ctx->aborted = true;
        return 0;   // signals curl to abort with CURLE_WRITE_ERROR
    }
    return total;
}

#ifdef __WIIU__
// The console's default socket buffers are small, and segments were arriving
// at 0.5 to 1.9 Mbit/s against a 2.5 Mbit/s stream: a fifth of realtime,
// while the server was serving at more than ten times realtime. A larger
// receive buffer and window scaling are what the Wii U's stack needs to move
// data at any speed.
int ConfigureSocket(void*, curl_socket_t socket, curlsocktype)
{
    int receiveBuffer = 256 * 1024;
    setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof(receiveBuffer));

    // Window scaling: without it the TCP window caps throughput well below
    // what the link can carry.
    int enable = 1;
    setsockopt(socket, SOL_SOCKET, SO_WINSCALE, &enable, sizeof(enable));

    // Segments are fetched as one long read, so waiting to coalesce small
    // writes only adds latency.
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

    return CURL_SOCKOPT_OK;
}
#endif

// Lets a transfer be cut short. Returning non-zero makes curl abort.
int AbortProgressCallback(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return g_abort.load() ? 1 : 0;
}

// Applies the settings every request shares.
void ApplyCommonOptions(CURL* curl, const std::string& url)
{
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, AbortProgressCallback);

#ifdef __WIIU__
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, ConfigureSocket);
    // curl's default receive buffer is 16KB, which means a great many small
    // reads through a stack that is slow per call.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
#endif

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // gzip if built in
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kAppUserAgent);

    if (!g_caBundlePath.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_caBundlePath.c_str());
    }
#ifdef __WIIU__
    // The console has no clock guarantee before the user sets it, and a
    // skewed clock makes every certificate look expired. Verifying the host
    // name still catches the case that matters on a LAN.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, g_caBundlePath.empty() ? 0L : 1L);
#endif
}

curl_slist* BuildHeaderList(const HttpHeaders& headers)
{
    curl_slist* list = nullptr;
    for (const HttpHeader& h : headers) {
        const std::string line = h.name + ": " + h.value;
        list = curl_slist_append(list, line.c_str());
    }
    return list;
}

// Accumulates the response headers so a session cookie can be picked out.
size_t CollectHeaders(char* data, size_t size, size_t nmemb, void* userp)
{
    const size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(data, total);
    return total;
}

// "Set-Cookie: name=value; Path=/" reduces to what a Cookie header needs.
std::string FirstSetCookie(const std::string& headers)
{
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        if (eol == std::string::npos) eol = headers.size();

        std::string line = headers.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        std::string lower = line.substr(0, 11);
        for (char& ch : lower) {
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        }
        if (lower != "set-cookie:") continue;

        std::string value = line.substr(11);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        const size_t semi = value.find(';');
        if (semi != std::string::npos) value = value.substr(0, semi);
        if (!value.empty()) return value;
    }
    return "";
}

HttpResponse Perform(CURL* curl, curl_slist* headerList)
{
    HttpResponse resp;
    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.error = curl_easy_strerror(rc);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status);
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return resp;
}

}  // namespace

namespace Http {

bool Init()
{
    static std::once_flag once;
    static bool result = false;

    std::call_once(once, [] {
#ifdef __WIIU__
        // Bring the console's network interface up before curl touches a
        // socket. These return NNResult, not an errno-style int.
        if (NNResult_IsFailure(ACInitialize())) {
            LOGF("[http] ACInitialize failed");
            return;
        }

        // Aroma usually has the link up already; only dial if it is not.
        // ACConnectWithConfigId blocks for seconds on a cold association.
        BOOL connected = FALSE;
        if (NNResult_IsFailure(ACIsApplicationConnected(&connected)) || !connected) {
            ACConfigId configId = 0;
            if (NNResult_IsFailure(ACGetStartupId(&configId))) {
                LOGF("[http] no network config on this console");
                return;
            }
            if (NNResult_IsFailure(ACConnectWithConfigId(configId))) {
                LOGF("[http] could not join the network");
                return;
            }
        }

        uint32_t ip = 0;
        if (NNResult_IsSuccess(ACGetAssignedAddress(&ip))) {
            LOGF("[http] console address %u.%u.%u.%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                 (ip >> 8) & 0xFF, ip & 0xFF);
        }
#endif
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            LOGF("[http] curl_global_init failed");
            return;
        }

        const std::string bundle = Platform::CaBundlePath();
        if (!bundle.empty() && Platform::FileExists(bundle)) {
            g_caBundlePath = bundle;
        }

        g_initialized = true;
        result = true;
    });

    return result;
}

void Shutdown()
{
    if (!g_initialized) return;
    curl_global_cleanup();
#ifdef __WIIU__
    ACFinalize();
#endif
    g_initialized = false;
}

void RequestAbort()
{
    g_abort = true;
}

bool AbortRequested()
{
    return g_abort.load();
}

void SetTimeoutSeconds(long seconds)
{
    if (seconds > 0) g_timeoutSeconds = seconds;
}

// Everything the three string-returning calls set up the same way.
struct StringRequest {
    CURL*        curl = nullptr;
    curl_slist*  list = nullptr;
    std::string  body;
    std::string  rawHeaders;

    bool begin(const std::string& url, const HttpHeaders& headers)
    {
        curl = curl_easy_init();
        if (!curl) return false;
        ApplyCommonOptions(curl, url);
        list = BuildHeaderList(headers);
        if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CollectHeaders);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rawHeaders);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        return true;
    }

    HttpResponse finish()
    {
        HttpResponse done = Perform(curl, list);
        done.body      = std::move(body);
        done.setCookie = FirstSetCookie(rawHeaders);
        return done;
    }
};

HttpResponse Get(const std::string& url, const HttpHeaders& headers)
{
    StringRequest request;
    if (!request.begin(url, headers)) {
        HttpResponse resp;
        resp.error = "curl_easy_init failed";
        return resp;
    }
    return request.finish();
}

HttpResponse Post(const std::string& url,
                  const std::string& body,
                  const HttpHeaders& headers)
{
    StringRequest request;
    if (!request.begin(url, headers)) {
        HttpResponse resp;
        resp.error = "curl_easy_init failed";
        return resp;
    }
    curl_easy_setopt(request.curl, CURLOPT_POST, 1L);
    curl_easy_setopt(request.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(request.curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    return request.finish();
}

HttpResponse Delete(const std::string& url, const HttpHeaders& headers)
{
    StringRequest request;
    if (!request.begin(url, headers)) {
        HttpResponse resp;
        resp.error = "curl_easy_init failed";
        return resp;
    }
    curl_easy_setopt(request.curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    return request.finish();
}

bool GetToFile(const std::string& url,
               const std::string& path,
               const HttpHeaders& headers)
{
    const std::string tmp = path + ".part";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) return false;

    CURL* curl = curl_easy_init();
    if (!curl) { std::fclose(fp); std::remove(tmp.c_str()); return false; }

    ApplyCommonOptions(curl, url);
    curl_slist* list = BuildHeaderList(headers);
    if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

    HttpResponse done = Perform(curl, list);
    std::fclose(fp);

    if (!done.ok()) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

HttpResponse GetStreaming(const std::string& url,
                          const HttpSink& sink,
                          const HttpHeaders& headers)
{
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl_easy_init failed"; return resp; }

    SinkCtx ctx{ &sink, false };
    ApplyCommonOptions(curl, url);
    curl_slist* list = BuildHeaderList(headers);
    if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToSink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    HttpResponse done = Perform(curl, list);
    // A caller-requested stop is not a failure; report it as such.
    if (ctx.aborted) done.error.clear();
    return done;
}

std::string UrlEncode(const std::string& in)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        const bool unreserved = (c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

}  // namespace Http
