// SPDX-License-Identifier: GPL-2.0-or-later

// http.h: minimal HTTP client surface used by the Jellyfin layer.
//
// One libcurl-backed implementation serves both targets: desktop links the
// system libcurl, the Wii U links devkitPro's wiiu-curl. Everything platform
// specific (bringing the console's network stack up, CA bundle location)
// lives behind Http::Init.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct HttpHeader {
    std::string name;
    std::string value;
};

using HttpHeaders = std::vector<HttpHeader>;

struct HttpResponse {
    long        status = 0;
    std::string body;
    std::string error;    // transport-level failure, empty on success

    // What the server said it would send, or -1 if it did not say.
    long long   contentLength = -1;

    // First byte of a 206, from Content-Range, or -1 when absent. A server
    // may answer from somewhere other than where it was asked.
    long long   rangeStart = -1;

    // A body cut short still carries 200, so error matters as much as status.
    bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

// Streaming sink: return false to abort the transfer (used to stop playback
// downloads promptly instead of waiting for the server to finish).
using HttpSink = std::function<bool(const uint8_t* data, size_t len)>;

namespace Http {

// Brings up the platform network stack and shared curl state. Safe to call
// more than once. Returns false if the console could not get a connection.
bool Init();
void Shutdown();

// Aborts every transfer now in flight and makes new ones fail immediately.
// The console gives an app only a moment to release the foreground when the
// user presses HOME, which is not long enough to wait out a slow download.
void RequestAbort();
bool AbortRequested();

// Wall-clock ceiling for a whole request. The Wii U's wifi is slow enough
// that the curl default is too aggressive for large library fetches.
void SetTimeoutSeconds(long seconds);

HttpResponse Get(const std::string& url, const HttpHeaders& headers = {});
HttpResponse Post(const std::string& url,
                  const std::string& body,
                  const HttpHeaders& headers = {});

HttpResponse Delete(const std::string& url, const HttpHeaders& headers = {});

// Downloads to a file. Writes to <path>.part then renames, so an interrupted
// transfer never leaves a truncated image in the art cache.
bool GetToFile(const std::string& url,
               const std::string& path,
               const HttpHeaders& headers = {});

// Streams a response through the sink without buffering the whole body.
HttpResponse GetStreaming(const std::string& url,
                          const HttpSink& sink,
                          const HttpHeaders& headers = {});

// Percent-encodes a value for use in a query string.
std::string UrlEncode(const std::string& in);

}  // namespace Http
