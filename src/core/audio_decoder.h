// SPDX-License-Identifier: GPL-2.0-or-later

// MP3 to PCM. Jellyfin is asked for MP3 because both consoles have a decoder
// for it and neither has one for AAC.
//
// mpg123 on the Wii U and the desktop, minimp3 on the Xbox 360, which has no
// mpg123 build.

#pragma once

// Neither Xbox has an mpg123 build.
#if defined(_XBOX)
  #define BJ_AUDIO_MPG123 0
#else
  #define BJ_AUDIO_MPG123 1
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Mp3Decoder {
public:
    // Receives interleaved signed 16-bit samples.
    using PcmFn = std::function<void(const uint8_t* pcm, size_t bytes)>;

    Mp3Decoder() = default;
    ~Mp3Decoder();

    Mp3Decoder(const Mp3Decoder&) = delete;
    Mp3Decoder& operator=(const Mp3Decoder&) = delete;

    bool open(std::string& error);
    void close();

    // Feeds compressed bytes, which need not be frame aligned. Decoded audio
    // comes out through the callback, and may be nothing if the decoder is
    // still assembling a frame.
    bool decode(const uint8_t* data, size_t length, const PcmFn& onPcm);

    // Valid once the first frame has been decoded; zero before that, since
    // the format is read from the stream rather than assumed.
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    bool formatKnown() const { return sampleRate_ > 0; }

private:
    // A plain member, not a thread_local static. elf2rpl cannot process the
    // TLS relocations that thread_local generates and fails the whole link
    // with "Unsupported relocation type 72", so thread-local storage is off
    // the table on this target.
    std::vector<uint8_t> scratch_;

    // mpg123_handle on one implementation, an mp3dec_t on the other. Opaque
    // here so this header needs neither library's own.
    void* handle_ = nullptr;
    int   sampleRate_ = 0;
    int   channels_   = 0;
};
