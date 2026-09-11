// SPDX-License-Identifier: GPL-2.0-or-later

// audio_decoder_mpg123.cpp: the mpg123 implementation, where it is packaged.

#include "core/audio_decoder.h"

#include "core/log.h"

#if BJ_AUDIO_MPG123

#include <mpg123.h>

#include <vector>

namespace {

// mpg123_init is global and must happen once per process.
bool EnsureLibraryInitialized()
{
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        initialized = true;
        ok = (mpg123_init() == MPG123_OK);
        if (!ok) LOGF("[audio] mpg123_init failed");
    }
    return ok;
}

}  // namespace

Mp3Decoder::~Mp3Decoder()
{
    close();
}

bool Mp3Decoder::open(int streamType, std::string& error)
{
    streamType_ = streamType;   // only one thing to decode here
    close();

    if (!EnsureLibraryInitialized()) { error = "Could not start the audio decoder"; return false; }

    int err = MPG123_OK;
    mpg123_handle* handle = mpg123_new(nullptr, &err);
    if (!handle) { error = "Could not create the audio decoder"; return false; }

    // Feed mode: bytes arrive from the demuxer in whatever sizes the
    // transport stream happened to use, not whole MP3 frames.
    if (mpg123_open_feed(handle) != MPG123_OK) {
        mpg123_delete(handle);
        error = "Could not open the audio decoder";
        return false;
    }

    // Signed 16-bit is what the mixer wants; sample rate and channel count
    // are whatever the stream turns out to be.
    mpg123_format_none(handle);
    static const long kRates[] = { 8000, 11025, 12000, 16000, 22050, 24000,
                                   32000, 44100, 48000 };
    for (long rate : kRates) {
        mpg123_format(handle, rate, MPG123_MONO | MPG123_STEREO,
                      MPG123_ENC_SIGNED_16);
    }

    handle_ = handle;
    return true;
}

void Mp3Decoder::close()
{
    if (!handle_) return;
    mpg123_handle* handle = static_cast<mpg123_handle*>(handle_);
    mpg123_close(handle);
    mpg123_delete(handle);
    handle_ = nullptr;
    sampleRate_ = 0;
    channels_   = 0;
}

bool Mp3Decoder::decode(const uint8_t* data, size_t length, const PcmFn& onPcm)
{
    if (!handle_ || !data || length == 0) return false;
    mpg123_handle* handle = static_cast<mpg123_handle*>(handle_);

    // Reused across calls so steady-state decoding does not allocate.
    if (scratch_.size() < 16384) scratch_.resize(16384);
    std::vector<uint8_t>& out = scratch_;

    size_t decoded = 0;
    int result = mpg123_decode(handle, data, length, out.data(), out.size(), &decoded);

    for (;;) {
        if (result == MPG123_NEW_FORMAT) {
            long rate = 0;
            int channels = 0, encoding = 0;
            mpg123_getformat(handle, &rate, &channels, &encoding);
            sampleRate_ = (int)rate;
            channels_   = channels;
            LOGF("[audio] %ld Hz, %d channel(s)", rate, channels);
        } else if (result == MPG123_ERR) {
            LOGF("[audio] decode error: %s", mpg123_strerror(handle));
            return false;
        }

        if (decoded > 0) onPcm(out.data(), decoded);

        // MPG123_OK means there may be more already buffered; NEED_MORE means
        // it has consumed everything and wants the next chunk.
        if (result == MPG123_NEED_MORE) break;

        decoded = 0;
        result = mpg123_decode(handle, nullptr, 0, out.data(), out.size(), &decoded);
        if (result == MPG123_NEED_MORE && decoded == 0) break;
    }
    return true;
}

#endif  // BJ_AUDIO_MPG123
