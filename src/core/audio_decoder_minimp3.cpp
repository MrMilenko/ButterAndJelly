// SPDX-License-Identifier: GPL-2.0-or-later

// audio_decoder_minimp3.cpp: MP3 without a library, for consoles with no mpg123.
//
// minimp3 decodes exactly one frame per call and reports how many bytes it
// consumed, which is what feeding a stream in whatever sized pieces the
// network hands over needs. Bytes that do not yet make a whole frame stay in
// the scratch buffer until the next call brings the rest.

#include "core/audio_decoder.h"

#if !BJ_AUDIO_MPG123 && !BJ_AUDIO_AAC

#include "core/log.h"

#include <cstring>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3        // no MP1 or MP2; a Jellyfin transcode is neither
#include "minimp3.h"

namespace {

// A frame is at most 1152 samples of two channels.
constexpr size_t kMaxFrameSamples = MINIMP3_MAX_SAMPLES_PER_FRAME;

// Enough to hold any frame plus the bytes of the next one that arrived with
// it. Beyond this the stream is not MP3 and the scratch is dropped rather
// than grown without limit.
constexpr size_t kScratchLimit = 64 * 1024;

}  // namespace

Mp3Decoder::~Mp3Decoder()
{
    close();
}

bool Mp3Decoder::open(int streamType, std::string& error)
{
    streamType_ = streamType;   // only one thing to decode here
    close();

    mp3dec_t* decoder = new mp3dec_t;
    mp3dec_init(decoder);

    handle_ = decoder;
    sampleRate_ = 0;
    channels_   = 0;
    scratch_.clear();
    error.clear();
    return true;
}

void Mp3Decoder::close()
{
    if (handle_) {
        delete (mp3dec_t*)handle_;
        handle_ = nullptr;
    }
    scratch_.clear();
    scratch_.shrink_to_fit();
    sampleRate_ = 0;
    channels_   = 0;
}

bool Mp3Decoder::decode(const uint8_t* data, size_t length, const PcmFn& onPcm)
{
    if (!handle_) return false;
    mp3dec_t* decoder = (mp3dec_t*)handle_;

    if (data && length) scratch_.insert(scratch_.end(), data, data + length);

    mp3d_sample_t pcm[kMaxFrameSamples];
    size_t at = 0;

    for (;;) {
        const size_t available = scratch_.size() - at;
        if (available == 0) break;

        mp3dec_frame_info_t info;
        std::memset(&info, 0, sizeof(info));
        const int samples = mp3dec_decode_frame(decoder, scratch_.data() + at,
                                                (int)available, pcm, &info);

        if (info.frame_bytes == 0) {
            // Not enough bytes for a whole frame yet. Keep what is left and
            // wait for the next feed.
            break;
        }

        at += (size_t)info.frame_bytes;

        if (samples > 0) {
            // The format comes from the stream rather than being assumed, and
            // is only known once a frame has actually decoded.
            if (sampleRate_ == 0) {
                sampleRate_ = info.hz;
                channels_   = info.channels;
                LOGF("[audio] %d Hz, %d channel(s), %d kbps",
                     info.hz, info.channels, info.bitrate_kbps);
            }
            const size_t bytes = (size_t)samples * (size_t)info.channels *
                                 sizeof(mp3d_sample_t);
            onPcm((const uint8_t*)pcm, bytes);
        }
        // samples == 0 with frame_bytes > 0 is a frame minimp3 skipped, an
        // ID3 tag or a junk header. Consuming it is the whole response.
    }

    if (at > 0) scratch_.erase(scratch_.begin(), scratch_.begin() + (long)at);

    if (scratch_.size() > kScratchLimit) {
        // Nothing in that much data decoded, so it is not an MP3 stream.
        // Dropping it beats growing forever on a bad transcode.
        LOGF("[audio] no frame in %lu bytes; dropping the buffer",
             (unsigned long)scratch_.size());
        scratch_.clear();
        return false;
    }
    return true;
}

#endif  // !BJ_AUDIO_MPG123 && !BJ_AUDIO_AAC
