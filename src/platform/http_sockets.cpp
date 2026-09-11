// SPDX-License-Identifier: GPL-2.0-or-later

// core/http.h over Berkeley sockets, for platforms with no libcurl.
//
// HTTP/1.1 GET, POST and DELETE, one connection per request. No TLS: an
// https:// address fails with a clear message rather than pretending.

#include "core/http.h"

#include "core/log.h"
#include "core/platform.h"

#include <SDL.h>
#include "core/text.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_XBOX)
  #include <xtl.h>
  #include <winsockx.h>
  using socket_t = SOCKET;
  static const socket_t kInvalidSocket = INVALID_SOCKET;
  #define BJ_CLOSE_SOCKET closesocket
  using bj_socklen_t = int;
  // Winsock's own value; the console's winsockx.h stops at SO_TYPE and never
  // declares this one, though getsockopt answers it.
  #ifndef SO_ERROR
    #define SO_ERROR 0x1007
  #endif
  // Undocumented. Without it the 360 encrypts the traffic and drops the
  // replies, reporting success throughout. The original Xbox rejects it with
  // WSAENOPROTOOPT and needs only the startup flag.
  #if defined(_XENON)
    #define BJ_SO_UNENCRYPTED 0x5801
  #endif
#else
  #error "http_sockets.cpp is for platforms without libcurl"
#endif

namespace {

std::atomic<bool> g_abort{ false };
bool g_initialized  = false;
// A server transcoding at about realtime can take longer than half a minute
// to produce a three second segment, and cutting it off there truncates it.
long g_timeoutSeconds = 60;

constexpr int  kConnectTimeoutMs = 10000;
constexpr int  kMaxRedirects     = 5;
constexpr size_t kReadChunk      = 32 * 1024;

// Everything the request path needs out of a URL.
struct ParsedUrl {
    std::string host;
    std::string pathAndQuery = "/";
    int         port = 80;
    bool        tls = false;
};

bool ParseUrl(const std::string& url, ParsedUrl& out)
{
    size_t at = 0;

    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        const std::string name = url.substr(0, scheme);
        if      (name == "http")  { out.tls = false; out.port = 80;  }
        else if (name == "https") { out.tls = true;  out.port = 443; }
        else return false;
        at = scheme + 3;
    }

    const size_t pathAt = url.find('/', at);
    std::string authority = (pathAt == std::string::npos)
        ? url.substr(at) : url.substr(at, pathAt - at);
    out.pathAndQuery = (pathAt == std::string::npos) ? "/" : url.substr(pathAt);
    if (out.pathAndQuery.empty()) out.pathAndQuery = "/";

    // No IPv6 literals: the console's stack does not carry an IPv6 address
    // this client could reach a server on, so a bracketed host is not a case
    // worth writing.
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        const std::string portText = authority.substr(colon + 1);
        if (!portText.empty() &&
            portText.find_first_not_of("0123456789") == std::string::npos) {
            out.port = std::atoi(portText.c_str());
            authority = authority.substr(0, colon);
        }
    }

    out.host = authority;
    return !out.host.empty() && out.port > 0 && out.port <= 65535;
}

// Numeric addresses resolve without touching the network, which is the common
// case here: a server found by UDP discovery is already an address.
bool ResolveHost(const std::string& host, IN_ADDR& out, std::string& error)
{
    const unsigned long numeric = inet_addr(host.c_str());
    if (numeric != INADDR_NONE) {
        out.s_addr = numeric;
        return true;
    }

    // The XDK has no gethostbyname; DNS goes through XNet, which answers
    // asynchronously and reports WSAEINPROGRESS until it is done.
    XNDNS* dns = nullptr;
    if (XNetDnsLookup(host.c_str(), nullptr, &dns) != 0 || !dns) {
        error = "could not start a DNS lookup for " + host;
        return false;
    }

    const DWORD deadline = GetTickCount() + 5000;
    while (dns->iStatus == WSAEINPROGRESS && GetTickCount() < deadline) {
        Sleep(20);
    }

    bool ok = false;
    if (dns->iStatus == 0 && dns->cina > 0) {
        out = dns->aina[0];
        ok = true;
    } else {
        error = "could not resolve " + host;
    }
    XNetDnsRelease(dns);
    return ok;
}

void SetBlocking(socket_t s, bool blocking)
{
    unsigned long nonblocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
}

// Waits until the socket is readable or writable, or the deadline passes.
// A failed connect lands in exceptfds here, never writefds, so watching
// writefds alone turns "refused" into a ten second timeout.
enum class Ready { Read, Write };
bool WaitReady(socket_t s, Ready which, DWORD deadline, bool* failed = nullptr)
{
    if (failed) *failed = false;

    for (;;) {
        if (g_abort.load()) return false;

        const DWORD now = GetTickCount();
        if (now >= deadline) return false;

        DWORD remaining = deadline - now;
        if (remaining > 1000) remaining = 1000;

        fd_set set, except;
        FD_ZERO(&set);
        FD_SET(s, &set);
        FD_ZERO(&except);
        FD_SET(s, &except);
        timeval tv;
        tv.tv_sec  = (long)(remaining / 1000);
        tv.tv_usec = (long)((remaining % 1000) * 1000);

        const int count = (which == Ready::Read)
            ? select(0, &set, nullptr, &except, &tv)
            : select(0, nullptr, &set, &except, &tv);

        if (count < 0) return false;
        if (count > 0) {
            if (FD_ISSET(s, &except)) {
                if (failed) *failed = true;
                return false;
            }
            return true;
        }
        // count == 0 is this second's timeout, not the request's; go round.
    }
}

// The error a pending connect finally settled on.
int PendingSocketError(socket_t s)
{
    int value = 0;
    bj_socklen_t length = sizeof(value);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&value, &length) != 0) return 0;
    return value;
}

socket_t Connect(const ParsedUrl& url, std::string& error)
{
    IN_ADDR address;
    if (!ResolveHost(url.host, address, error)) return kInvalidSocket;

    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) { error = "could not create a socket"; return kInvalidSocket; }

#if defined(BJ_SO_UNENCRYPTED)
    {
        BOOL unencrypted = TRUE;
        if (setsockopt(s, SOL_SOCKET, BJ_SO_UNENCRYPTED,
                       (const char*)&unencrypted, sizeof(unencrypted)) != 0) {
            LOGF("[http] could not mark the socket unencrypted: %d", WSAGetLastError());
        }
    }
#endif

    // A bigger receive buffer so a segment arrives near line rate, and no
    // Nagle delay. The original Xbox's pool is fixed at XNetStartup, and
    // asking for more than it holds loses packets rather than enlarging it.
#if defined(_XBOX) && !defined(_XENON)
    int receiveBuffer = 64 * 1024;
#else
    int receiveBuffer = 256 * 1024;
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&receiveBuffer, sizeof(receiveBuffer));
    int enable = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&enable, sizeof(enable));

    sockaddr_in remote;
    std::memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port   = htons((u_short)url.port);
    remote.sin_addr   = address;

    SetBlocking(s, false);
    const int rc = connect(s, (sockaddr*)&remote, sizeof(remote));
    if (rc != 0) {
        // WSAEWOULDBLOCK here is the normal answer for a non-blocking
        // connect, not a problem, and every request makes one.
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEALREADY) {
            error = "could not connect to " + url.host;
            BJ_CLOSE_SOCKET(s);
            return kInvalidSocket;
        }
        bool refused = false;
        if (!WaitReady(s, Ready::Write, GetTickCount() + kConnectTimeoutMs, &refused)) {
            // select() on this stack only ever times out, so ask the socket
            // itself: a connected one answers getpeername.
            sockaddr_in peer;
            bj_socklen_t peerLength = sizeof(peer);
            if (getpeername(s, (sockaddr*)&peer, &peerLength) == 0) {
                LOGF("[http] connected to %s, but select() never said so",
                     url.host.c_str());
                SetBlocking(s, true);
                return s;
            }

            if (g_abort.load()) {
                error = "aborted";
            } else if (refused) {
                char detail[96];
                SDL_snprintf(detail, sizeof(detail), " (error %d)", PendingSocketError(s));
                error = "could not connect to " + url.host + detail;
            } else {
                char detail[96];
                SDL_snprintf(detail, sizeof(detail), " (error %d)", PendingSocketError(s));
                error = "timed out connecting to " + url.host + detail;
            }
            BJ_CLOSE_SOCKET(s);
            return kInvalidSocket;
        }
    }
    SetBlocking(s, true);

    // Reads are bounded by the socket's own timeout rather than by select, for
    // the same reason. A second at a time, so an abort is still noticed.
    {
        int timeout = 1000;   // milliseconds, which is how Winsock spells it
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    }
    return s;
}

// A socket with a read-ahead buffer, so header lines can be pulled off one at
// a time without a syscall per byte, and whatever body bytes came along with
// them are still there afterwards.
class Reader {
public:
    Reader(socket_t s, DWORD deadline) : s_(s), deadline_(deadline) {}

    // Reads one CRLF-terminated line, without the terminator.
    bool line(std::string& out)
    {
        out.clear();
        for (;;) {
            while (pos_ < buf_.size()) {
                const char c = buf_[pos_++];
                if (c == '\n') {
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    return true;
                }
                out += c;
                if (out.size() > 8192) return false;   // no sane header is this long
            }
            if (!fill()) return false;
        }
    }

    // Fills up to `want` bytes. Returns how many, 0 at end of stream.
    size_t read(uint8_t* out, size_t want)
    {
        if (pos_ >= buf_.size() && !fill()) return 0;
        const size_t available = buf_.size() - pos_;
        const size_t take = (want < available) ? want : available;
        std::memcpy(out, buf_.data() + pos_, take);
        pos_ += take;
        return take;
    }

    bool aborted() const { return aborted_; }

    // Why the last fill() gave up.
    const char* stopReason() const { return stop_; }
    int  lastError() const { return lastError_; }

    const char* stop_ = "none";
    int lastError_ = 0;

private:
    bool fill()
    {
        // SO_RCVTIMEO bounds each attempt at a second, so this loop is what
        // enforces the request's deadline and what notices an abort.
        for (;;) {
            if (g_abort.load()) { aborted_ = true; stop_ = "aborted"; return false; }
            if (GetTickCount() >= deadline_) { stop_ = "deadline"; return false; }

            buf_.resize(kReadChunk);
            const int got = recv(s_, (char*)buf_.data(), (int)buf_.size(), 0);
            if (got > 0) {
                buf_.resize((size_t)got);
                pos_ = 0;
                return true;
            }
            buf_.clear();
            pos_ = 0;
            if (got == 0) { stop_ = "server closed"; return false; }

            const int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) {
                stop_ = "socket error";
                lastError_ = err;
                return false;
            }
            // Otherwise this second produced nothing; go round.
        }
    }

    socket_t s_;
    DWORD    deadline_;
    std::vector<uint8_t> buf_;
    size_t   pos_ = 0;
    bool     aborted_ = false;
};

bool SendAll(socket_t s, const char* data, size_t length, DWORD deadline)
{
    size_t sent = 0;
    while (sent < length) {
        if (g_abort.load()) return false;
        if (GetTickCount() >= deadline) return false;

        const int wrote = send(s, data + sent, (int)(length - sent), 0);
        if (wrote > 0) { sent += (size_t)wrote; continue; }

        const int err = WSAGetLastError();
        if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK) return false;
    }
    return true;
}

// The XDK's CRT is C89 and has no strtoll, and a Content-Length can exceed
// what a long holds on a 32-bit machine.
long long ParseInt64(const std::string& text)
{
    long long value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::string Lowercased(const std::string& in)
{
    std::string out = in;
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

// The caller-facing body handler. Returning false stops the transfer, which
// is how playback drops a segment it no longer wants.
using Writer = std::function<bool(const uint8_t*, size_t)>;

// Reads `length` bytes of body, or to end of stream when length is negative.
bool PumpBody(Reader& reader, long long length, const Writer& write)
{
    uint8_t chunk[kReadChunk];
    long long remaining = length;

    while (length < 0 || remaining > 0) {
        size_t want = sizeof(chunk);
        if (length >= 0 && (long long)want > remaining) want = (size_t)remaining;

        const size_t got = reader.read(chunk, want);
        if (got == 0) return length < 0;    // clean end only if we read to close
        if (!write(chunk, got)) return false;
        if (length >= 0) remaining -= (long long)got;
    }
    return true;
}

bool PumpChunked(Reader& reader, const Writer& write)
{
    for (;;) {
        std::string header;
        if (!reader.line(header)) return false;

        // "1a2b" or "1a2b;ext=value"; the extension is not ours to care about.
        const size_t semicolon = header.find(';');
        if (semicolon != std::string::npos) header = header.substr(0, semicolon);
        const long size = std::strtol(header.c_str(), nullptr, 16);
        if (size < 0) return false;
        if (size == 0) break;

        long remaining = size;
        uint8_t chunk[kReadChunk];
        while (remaining > 0) {
            size_t want = sizeof(chunk);
            if ((long)want > remaining) want = (size_t)remaining;
            const size_t got = reader.read(chunk, want);
            if (got == 0) return false;
            if (!write(chunk, got)) return false;
            remaining -= (long)got;
        }

        std::string blank;
        if (!reader.line(blank)) return false;   // the CRLF after the chunk
    }

    // Trailers, then the blank line that ends them.
    std::string trailer;
    while (reader.line(trailer) && !trailer.empty()) {}
    return true;
}

// Resolves a Location against the request it answers, which may be absolute,
// rooted, or relative.
std::string ResolveRedirect(const ParsedUrl& from, const std::string& location)
{
    if (location.find("://") != std::string::npos) return location;

    const std::string base = (from.tls ? "https://" : "http://") + from.host +
        ((from.port == (from.tls ? 443 : 80)) ? "" : (":" + bj::ToString(from.port)));

    if (!location.empty() && location[0] == '/') return base + location;

    std::string directory = from.pathAndQuery;
    const size_t slash = directory.rfind('/');
    directory = (slash == std::string::npos) ? "/" : directory.substr(0, slash + 1);
    return base + directory + location;
}

HttpResponse Perform(const char* method, const std::string& url,
                     const std::string* body, const HttpHeaders& headers,
                     const Writer& write, int redirectsLeft)
{
    HttpResponse response;

    if (!g_initialized) { response.error = "the network is not up"; return response; }
    if (g_abort.load()) { response.error = "aborted"; return response; }

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed)) { response.error = "could not parse " + url; return response; }
    if (parsed.tls) {
        response.error = "this console cannot do HTTPS yet; use an http:// address";
        return response;
    }

    const DWORD deadline = GetTickCount() + (DWORD)(g_timeoutSeconds * 1000);

    socket_t s = Connect(parsed, response.error);
    if (s == kInvalidSocket) return response;

    std::string request;
    request.reserve(512 + (body ? body->size() : 0));
    request += method; request += ' '; request += parsed.pathAndQuery;
    request += " HTTP/1.1\r\n";
    request += "Host: " + parsed.host;
    if (parsed.port != 80) request += ":" + bj::ToString(parsed.port);
    request += "\r\n";
    request += std::string("User-Agent: ") + kAppUserAgent + "\r\n";
    request += "Accept: */*\r\n";
    // No gzip: nothing here decompresses, and asking for what we cannot read
    // would be worse than the bytes saved.
    request += "Accept-Encoding: identity\r\n";
    request += "Connection: close\r\n";
    for (const HttpHeader& h : headers) request += h.name + ": " + h.value + "\r\n";
    if (body) request += "Content-Length: " + bj::ToString(body->size()) + "\r\n";
    request += "\r\n";
    if (body) request += *body;

    if (!SendAll(s, request.data(), request.size(), deadline)) {
        response.error = g_abort.load() ? "aborted" : "could not send the request";
        BJ_CLOSE_SOCKET(s);
        return response;
    }

    Reader reader(s, deadline);

    std::string statusLine;
    if (!reader.line(statusLine)) {
        response.error = reader.aborted() ? "aborted" : "no reply from the server";
        BJ_CLOSE_SOCKET(s);
        return response;
    }
    {
        const size_t space = statusLine.find(' ');
        if (space == std::string::npos) {
            response.error = "the server's reply was not HTTP";
            BJ_CLOSE_SOCKET(s);
            return response;
        }
        response.status = std::strtol(statusLine.c_str() + space + 1, nullptr, 10);
    }

    long long contentLength = -1;
    long long rangeStart = -1;
    bool chunked = false;
    std::string location;

    for (;;) {
        std::string header;
        if (!reader.line(header)) {
            response.error = reader.aborted() ? "aborted" : "the headers ended early";
            BJ_CLOSE_SOCKET(s);
            return response;
        }
        if (header.empty()) break;

        const size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = Lowercased(header.substr(0, colon));
        std::string value = header.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }

        if      (name == "content-length")    contentLength = ParseInt64(value);
        else if (name == "transfer-encoding") chunked = Lowercased(value).find("chunked") != std::string::npos;
        else if (name == "location")          location = value;
        else if (name == "set-cookie" && response.setCookie.empty()) {
            // The attributes after the semicolon are a browser's concern.
            const size_t semi = value.find(';');
            response.setCookie = (semi == std::string::npos) ? value
                                                             : value.substr(0, semi);
        }
        else if (name == "content-range") {
            // "bytes 1234-5678/9012". Only the first number matters here.
            const size_t space = value.find(' ');
            if (space != std::string::npos) {
                rangeStart = ParseInt64(value.substr(space + 1));
            }
        }
    }

    const bool isRedirect = (response.status == 301 || response.status == 302 ||
                             response.status == 303 || response.status == 307 ||
                             response.status == 308);
    if (isRedirect && !location.empty() && redirectsLeft > 0) {
        BJ_CLOSE_SOCKET(s);
        const std::string next = ResolveRedirect(parsed, location);
        // 303, and 301/302 in practice, turn everything into a GET.
        const bool keepMethod = (response.status == 307 || response.status == 308);
        return Perform(keepMethod ? method : "GET", next,
                       keepMethod ? body : nullptr, headers, write, redirectsLeft - 1);
    }

    // 204 and 304 carry no body whatever the headers say.
    response.contentLength = contentLength;
    response.rangeStart    = rangeStart;

    const bool bodyless = (response.status == 204 || response.status == 304);
    bool complete = true;
    size_t received = 0;
    const Writer count = [&](const uint8_t* data, size_t length) {
        received += length;
        return write(data, length);
    };
    if (!bodyless) {
        complete = chunked ? PumpChunked(reader, count)
                           : PumpBody(reader, contentLength, count);
    }

    BJ_CLOSE_SOCKET(s);

    // Jellyfin streams HLS segments chunked as the transcoder produces them,
    // so a body can end short without a Content-Length to check it against.
    if (!complete && !reader.aborted()) {
        char why[160];
        _snprintf(why, sizeof(why),
                  "incomplete body: %s after %lu of %s bytes (%s)",
                  reader.stopReason(), (unsigned long)received,
                  contentLength >= 0 ? bj::ToString((long)contentLength).c_str() : "unknown",
                  chunked ? "chunked" : "content-length");
        why[sizeof(why) - 1] = '\0';
        response.error = why;
        LOGF("[http] %s", why);
    } else if (reader.aborted()) {
        response.error = "aborted";
    }
    return response;
}

Writer AppendTo(std::string& out)
{
    return [&out](const uint8_t* data, size_t length) {
        out.append((const char*)data, length);
        return true;
    };
}

}  // namespace

namespace Http {

bool Init()
{
    if (g_initialized) return true;

    // The console's stack is brought up by Platform::Init, which runs before
    // anything here: XNetStartup has to happen before any socket call, and
    // the discovery code opens a socket without going through Http at all.
    g_initialized = true;
    LOGF("[http] sockets, no TLS: http:// only");
    return true;
}

void Shutdown() { g_initialized = false; }

void RequestAbort()   { g_abort.store(true); }
bool AbortRequested() { return g_abort.load(); }

void SetTimeoutSeconds(long seconds) { g_timeoutSeconds = seconds; }

HttpResponse Get(const std::string& url, const HttpHeaders& headers)
{
    HttpResponse response;
    std::string body;
    response = Perform("GET", url, nullptr, headers, AppendTo(body), kMaxRedirects);
    response.body = std::move(body);
    return response;
}

HttpResponse Post(const std::string& url, const std::string& body,
                  const HttpHeaders& headers)
{
    HttpResponse response;
    std::string out;
    response = Perform("POST", url, &body, headers, AppendTo(out), kMaxRedirects);
    response.body = std::move(out);
    return response;
}

HttpResponse Delete(const std::string& url, const HttpHeaders& headers)
{
    HttpResponse response;
    std::string out;
    response = Perform("DELETE", url, nullptr, headers, AppendTo(out), kMaxRedirects);
    response.body = std::move(out);
    return response;
}

bool GetToFile(const std::string& url, const std::string& path,
               const HttpHeaders& headers)
{
    // Written to <path>.part and renamed, so an interrupted transfer never
    // leaves a truncated image in the art cache.
    const std::string partial = path + ".part";

    const std::string native = Platform::NativePath(partial);
    std::FILE* file = std::fopen(native.c_str(), "wb");
    if (!file) {
        // Distinct from a failed request: the download never started because
        // there is nowhere to put it.
        LOGF("[http] cannot write %s", native.c_str());
        return false;
    }

    bool wrote = true;
    const HttpResponse response = Perform("GET", url, nullptr, headers,
        [file, &wrote](const uint8_t* data, size_t length) {
            if (std::fwrite(data, 1, length, file) != length) { wrote = false; return false; }
            return true;
        }, kMaxRedirects);
    std::fclose(file);

    if (!response.ok() || !wrote || !response.error.empty()) {
        std::remove(native.c_str());
        return false;
    }

    const std::string nativeFinal = Platform::NativePath(path);
    std::remove(nativeFinal.c_str());
    if (std::rename(native.c_str(), nativeFinal.c_str()) != 0) {
        std::remove(native.c_str());
        return false;
    }
    return true;
}

HttpResponse GetStreaming(const std::string& url, const HttpSink& sink,
                          const HttpHeaders& headers)
{
    bool stopped = false;
    HttpResponse response = Perform("GET", url, nullptr, headers,
        [&sink, &stopped](const uint8_t* data, size_t length) {
            if (!sink(data, length)) { stopped = true; return false; }
            return true;
        }, kMaxRedirects);

    // A caller-requested stop is not a failure; report it as such.
    if (stopped) response.error.clear();
    return response;
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
