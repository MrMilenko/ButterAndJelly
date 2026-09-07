// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/player.h"

#include "core/hls.h"
#include "core/http.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/ts_demux.h"
#include "core/video_decoder.h"
#include "core/thread.h"
#include "core/text.h"

#include <algorithm>
#include <cstdint>

namespace {

// The frame queue is sized by memory, not by a frame count.
//
// Forty frames was chosen at 480p, where it costs 29MB. The same count at
// 720p is 53MB, and the pool of retired frames can hold as many again, which
// is 100MB of decoded video on top of the decoder's own 37MB. Segments are
// buffered separately and cover the network, so the frame queue only has to
// smooth out decoding.
constexpr size_t kFrameQueueBytes = 20u * 1024 * 1024;
constexpr size_t kMinQueuedFrames = 12;
constexpr size_t kMaxQueuedFrames = 40;

// Enough buffered before the clock starts to survive a stutter, and more
// than the stream's reordering depth, since frames arrive in decode order
// and are sorted into place as they come.
constexpr size_t kMinFramesBeforeStart = 8;

// Whole segments held between the downloader and the decoder. Segments run
// about ten seconds, so this is nearly a minute of slack against the network.
constexpr size_t kMaxBufferedSegments = 5;


constexpr int64_t kTimescale = 90000;   // MPEG-TS PTS units per second

}  // namespace

Player::Player() = default;

Player::~Player()
{
    close();
}

bool Player::open(JellyfinClient& client, const std::string& itemId,
                  int maxHeight, double startSeconds, std::string& error)
{
    close();

    if (!client.signedIn()) { error = "Not signed in"; return false; }

    client_   = &client;
    stopping_ = false;
    paused_   = false;
    decoded_  = 0;
    dropped_  = 0;
    basePts_  = -1;
    duration_ = 0.0;
    queueLimit_ = 0;
    framesBeforeStart_ = kMinFramesBeforeStart;
    startOffsetSeconds_ = startSeconds > 0.0 ? startSeconds : 0.0;
    clockStartMs_  = 0;
    pausedAtMs_    = -1;
    pausedTotalMs_ = 0;
    {
        bj::ScopedLock lock(mutex_);
        frames_.clear();
        pool_.clear();
        error_.clear();
        playSessionId_.clear();
    }

    {
        bj::ScopedLock lock(segmentMutex_);
        segments_.clear();
        downloadComplete_ = false;
    }

    state_ = State::Opening;
    // Copied into the closure rather than captured by reference: open()
    // returns as soon as the threads are running, so nothing on its frame is
    // still alive by the time they read their arguments. `client` outlives
    // the player, so its address is safe to hold.
    JellyfinClient* clientPtr = &client;
    const std::string id = itemId;
    downloader_ = bj::Thread([this, clientPtr, id, maxHeight, startSeconds] {
        downloadThread(clientPtr, id, maxHeight, startSeconds);
    }, "bj-download");
    decoder_ = bj::Thread([this] { decodeThread(); }, "bj-decode");
    return true;
}

void Player::close()
{
    stopping_ = true;
    spaceCv_.notifyAll();
    segmentReadyCv_.notifyAll();
    segmentSpaceCv_.notifyAll();

    if (downloader_.joinable()) downloader_.join();
    if (decoder_.joinable())    decoder_.join();

    {
        bj::ScopedLock lock(segmentMutex_);
        segments_.clear();
        downloadComplete_ = false;
    }

    {
        bj::ScopedLock lock(audioMutex_);
        audio_.close();
        audioDecoder_.close();
    }
    {
        bj::ScopedLock lock(audioMutex_);
        audioStartPts_ = -1;
    }
    audioClockReady_.store(false);

    std::string session;
    {
        bj::ScopedLock lock(mutex_);
        frames_.clear();
        pool_.clear();
        session = playSessionId_;
        playSessionId_.clear();
    }

    // Leave no ffmpeg running on the server once we stop pulling segments.
    if (client_ && !session.empty()) client_->stopTranscode(session);

    client_ = nullptr;
    state_  = State::Idle;
}

void Player::setFailed(const std::string& message)
{
    LOGF("[player] failed: %s", message.c_str());
    {
        bj::ScopedLock lock(mutex_);
        error_ = message;
    }
    state_ = State::Failed;
}

std::string Player::errorText() const
{
    bj::ScopedLock lock(mutex_);
    return error_;
}

void Player::downloadThread(JellyfinClient* client, std::string itemId,
                            int maxHeight, double startSeconds)
{
    std::string error;

    JellyfinClient::PlaybackPlan plan;
    if (!client->playbackInfo(itemId, plan, error)) { setFailed(error); return; }

    {
        bj::ScopedLock lock(mutex_);
        playSessionId_ = plan.playSessionId;
    }
    displayAspect_ = plan.displayAspect;
    LOGF("[player] source %dx%d %s %s level %d, display aspect %s",
         plan.width, plan.height, plan.videoCodec.c_str(),
         plan.videoProfile.c_str(), plan.videoLevel,
         bj::ToString(plan.displayAspect, 3).c_str());

    const std::string masterUrl = client->videoHlsUrl(plan, maxHeight);
    if (masterUrl.empty()) { setFailed("Could not build a stream URL"); return; }

    HlsPlaylist playlist;
    if (!playlist.load(masterUrl, error)) { setFailed(error); return; }
    duration_ = playlist.totalDuration();

    // Seek by choosing a segment rather than with startTimeTicks: the server
    // still numbers the playlist from zero, so the offset does not move it.
    const std::vector<HlsSegment>& segments = playlist.segments();
    size_t firstSegment = 0;
    if (startSeconds > 0.0) {
        firstSegment = playlist.segmentAt(startSeconds);
        startOffsetSeconds_ = segments[firstSegment].startTime;
        LOGF("[player] starting at segment %lu of %lu, %ss in",
             (unsigned long)firstSegment, (unsigned long)segments.size(),
             bj::ToString(startOffsetSeconds_, 0).c_str());
    }

    for (size_t index = firstSegment; index < segments.size(); ++index) {
        const HlsSegment& segment = segments[index];
        if (stopping_) break;

        // Wait for room before fetching, so memory stays bounded.
        {
            bj::Lock lock(segmentMutex_);
            segmentSpaceCv_.wait(lock, [this] {
                return stopping_.load() || segments_.size() < kMaxBufferedSegments;
            });
            if (stopping_) break;
        }

        // A segment fetch can fail for reasons that pass: the console's wifi
        // dropping a moment, or the server still starting a transcode. Give
        // up only after several tries, since abandoning the film over one
        // bad request is far worse than a pause.
        std::vector<uint8_t> data;
        data.reserve(1 << 20);
        HttpResponse response;
        uint64_t fetchMs = 0;

        for (int attempt = 0; attempt < 4 && !stopping_; ++attempt) {
            data.clear();
            const uint64_t fetchStart = Platform::NowMs();
            response = Http::GetStreaming(
                segment.url,
                [&](const uint8_t* bytes, size_t length) {
                    if (stopping_) return false;
                    data.insert(data.end(), bytes, bytes + length);
                    return true;
                });
            fetchMs = Platform::NowMs() - fetchStart;

            if (stopping_) break;
            if (response.ok() && !data.empty()) break;

            LOGF("[net] segment %lu attempt %d failed (status %ld), retrying",
                 (unsigned long)index, attempt + 1, response.status);
            // Back off a little, mostly to give a transcode time to catch up.
            bj::SleepMs(300 * (attempt + 1));
        }

        // The server serves segments at many times realtime, so if fetching
        // one takes longer than the video it contains, the limit is here:
        // either the console's own network throughput or its CPU.
        if (fetchMs > 0) {
            LOGF("[net] segment %ss of video, %lu KB in %lums (%s Mbit/s, %sx realtime)",
                 bj::ToString(segment.duration, 1).c_str(),
                 (unsigned long)(data.size() / 1024),
                 (unsigned long)fetchMs,
                 bj::ToString((double)data.size() * 8.0 / (double)fetchMs / 1000.0, 1).c_str(),
                 bj::ToString(segment.duration * 1000.0 / (double)fetchMs, 1).c_str());
        }

        if (stopping_) break;
        if (!response.ok() || data.empty()) {
            LOGF("[player] segment %lu failed after retries (status %ld)",
                 (unsigned long)index, response.status);
            setFailed("The stream stopped unexpectedly");
            break;
        }

        {
            bj::ScopedLock lock(segmentMutex_);
            segments_.push_back(std::move(data));
        }
        segmentReadyCv_.notifyOne();
    }

    {
        bj::ScopedLock lock(segmentMutex_);
        downloadComplete_ = true;
    }
    segmentReadyCv_.notifyAll();
}

void Player::decodeThread()
{
    std::unique_ptr<VideoDecoder> decoder = VideoDecoder::Create();
    std::string error;
    if (!decoder->open(1920, 1080, error)) { setFailed(error); return; }
    LOGF("[player] decoder: %s", decoder->name());

    state_ = State::Buffering;

    TsDemuxer demux;
    auto onFrame = [this](const Nv12View& view) {
        decoded_.fetch_add(1);

        // Log the first handful of timestamps and count anything wrong after
        // that. A missing or out-of-order timestamp makes every frame look
        // overdue, which drops frames that were perfectly fine.
        if (view.pts < 0) {
            ptsMissing_.fetch_add(1);
        } else {
            if (lastSeenPts_ != INT64_MIN && view.pts < lastSeenPts_) {
                ptsBackwards_.fetch_add(1);
            }
            if (ptsLogged_ < 6) {
                LOGF("[player] frame %d pts %s (delta %s)", ptsLogged_,
                     bj::ToString((long long)view.pts).c_str(),
                     bj::ToString(lastSeenPts_ == INT64_MIN
                                      ? 0LL : (long long)(view.pts - lastSeenPts_)).c_str());
                ++ptsLogged_;
            }
            lastSeenPts_ = view.pts;
        }

        pushFrame(view);
    };
    std::string audioError;
    const bool audioReady = audioDecoder_.open(audioError);
    if (!audioReady) {
        // Not fatal: a silent film beats no film.
        LOGF("[audio] unavailable, playing without sound: %s", audioError.c_str());
    } else {
        LOGF("[audio] decoder open, waiting for a stream");
    }

    // Counted so the log can distinguish "no audio arrived" from "audio
    // arrived and would not decode".
    size_t audioSamples = 0, audioBytes = 0, pcmBytes = 0;
    bool   audioReported = false;

    auto onPcm = [&](const uint8_t* pcm, size_t bytes) {
        pcmBytes += bytes;
        bj::ScopedLock lock(audioMutex_);
        if (!audio_.isOpen()) {
            // The format is read from the stream, so the device cannot be
            // opened until the first frame has been decoded.
            if (!audioDecoder_.formatKnown()) return;
            std::string error;
            if (!audio_.open(audioDecoder_.sampleRate(),
                             audioDecoder_.channels(), error)) {
                LOGF("[audio] %s", error.c_str());
                return;
            }
        }
        audio_.queue(pcm, bytes);
    };

    auto onSample = [&](const TsSample& sample) {
        if (sample.video) {
            decoder->decode(sample.data.data(), sample.data.size(),
                            sample.pts, onFrame);
            return;
        }

        // The first audio timestamp anchors the clock everything else is
        // presented against.
        if (!audioClockReady_.load() && sample.pts >= 0) {
            {
                bj::ScopedLock lock(audioMutex_);
                audioStartPts_ = sample.pts;
            }
            audioClockReady_.store(true);
            LOGF("[audio] clock starts at pts %s", bj::ToString((long long)sample.pts).c_str());
        }
        ++audioSamples;
        audioBytes += sample.data.size();
        audioDecoder_.decode(sample.data.data(), sample.data.size(), onPcm);

        if (!audioReported && audioSamples >= 40) {
            audioReported = true;
            LOGF("[audio] %lu packets, %lu KB fed, %lu KB of PCM out",
                 (unsigned long)audioSamples, (unsigned long)(audioBytes / 1024),
                 (unsigned long)(pcmBytes / 1024));
        }
    };

    for (;;) {
        std::vector<uint8_t> segment;
        {
            bj::Lock lock(segmentMutex_);
            segmentReadyCv_.wait(lock, [this] {
                return stopping_.load() || !segments_.empty() || downloadComplete_;
            });
            if (stopping_) break;
            if (segments_.empty()) {
                if (downloadComplete_) break;
                continue;
            }
            segment = std::move(segments_.front());
            segments_.pop_front();
        }
        segmentSpaceCv_.notifyOne();

        demux.feed(segment.data(), segment.size(), onSample);

        if (!tablesReported_ && demux.tablesReady()) {
            tablesReported_ = true;
            LOGF("[demux] video pid %d type 0x%02X, audio pid %d type 0x%02X",
                 demux.videoPid(), demux.videoStreamType(),
                 demux.audioPid(), demux.audioStreamType());
            if (demux.audioPid() < 0) {
                LOGF("[demux] no audio stream in this transport stream");
            }
        }

        if (stopping_) break;
    }

    demux.flush(onSample);
    decoder->flush(onFrame);

    if (!stopping_) {
        LOGF("[player] stream complete: %d decoded, %d dropped, "
             "%d without a timestamp, %d arrived out of order, %d reordered",
             decoded_.load(), dropped_.load(),
             ptsMissing_.load(), ptsBackwards_.load(), reordered_.load());
        state_ = State::Ended;
    }
}

void Player::pushFrame(const Nv12View& view)
{
    // How many frames of this size fit the budget. Known only once a frame
    // has been seen, since it depends on what the server actually sent.
    const size_t frameBytes =
        (size_t)view.lumaStride * view.height * 3 / 2;
    if (frameBytes > 0 && queueLimit_ == 0) {
        size_t limit = kFrameQueueBytes / frameBytes;
        if (limit < kMinQueuedFrames) limit = kMinQueuedFrames;
        if (limit > kMaxQueuedFrames) limit = kMaxQueuedFrames;
        queueLimit_ = limit;
        framesBeforeStart_ = std::max(kMinFramesBeforeStart, limit / 3);
        LOGF("[player] %lu frame queue at %s MB each, %s MB total",
             (unsigned long)queueLimit_,
             bj::ToString((double)frameBytes / 1e6, 2).c_str(),
             bj::ToString((double)(queueLimit_ * frameBytes) / 1e6, 0).c_str());
    }

    Nv12Frame frame;
    {
        bj::Lock lock(mutex_);

        // Block while the queue is full: this is what paces the downloader
        // to playback speed instead of pulling the whole movie at line rate.
        spaceCv_.wait(lock, [this] {
            return stopping_.load() || frames_.size() < queueLimit_;
        });
        if (stopping_) return;

        if (!pool_.empty()) {
            frame = std::move(pool_.back());
            pool_.pop_back();
        }
    }

    // Copy outside the lock. This is the one unavoidable copy per frame --
    // the decoder is about to overwrite its buffer, and holding the mutex
    // across it would stall the thread trying to display a frame.
    frame.copyFrom(view);

    bj::ScopedLock lock(mutex_);
    if (stopping_) return;

    // Insert in timestamp order. B-frames decode out of presentation order
    // and the decoder emits each as it finishes, so arrival order is not
    // display order. Reordering depth is bounded by the DPB, so the walk
    // from the back is short.
    if (frame.pts < 0 || frames_.empty() || frames_.back().pts <= frame.pts) {
        frames_.push_back(std::move(frame));
    } else {
        auto at = frames_.end();
        while (at != frames_.begin()) {
            auto previous = at;
            --previous;
            if (previous->pts <= frame.pts) break;
            at = previous;
        }
        frames_.insert(at, std::move(frame));
        reordered_.fetch_add(1);
    }
}

bool Player::nextFrame(Nv12Frame& out)
{
    bj::Lock lock(mutex_);
    if (frames_.empty()) return false;

    const State current = state_.load();

    // Hold the first frames back until there is a cushion, then start.
    if (current == State::Buffering) {
        if (frames_.size() < framesBeforeStart_) return false;
        basePts_      = frames_.front().pts;
        clockStartMs_ = Platform::NowMs();
        pausedTotalMs_ = 0;
        pausedAtMs_    = -1;
        state_ = State::Playing;

        // Release the audio device now, so sound and picture start together
        // rather than audio running ahead through the whole pre-roll.
        {
            bj::ScopedLock audioLock(audioMutex_);
            audio_.setPaused(false);
        }
        LOGF("[player] starting playback at video pts %s",
             bj::ToString((long long)basePts_).c_str());
    } else if (current != State::Playing && current != State::Ended) {
        return false;
    }

    if (paused_) return false;

    // Prefer the audio clock. It is derived from what the device has
    // actually played, so video cannot drift away from the sound.
    int64_t elapsedMs;
    int64_t audioStart = -1;
    double  audioPlayed = 0.0;
    bool    audioUsable = false;
    if (audioClockReady_.load()) {
        bj::ScopedLock lock(audioMutex_);
        if (audio_.isOpen()) {
            audioStart  = audioStartPts_;
            audioPlayed = audio_.playedSeconds();
            audioUsable = true;
        }
    }

    if (audioUsable) {
        const int64_t audioPts =
            audioStart + (int64_t)(audioPlayed * (double)kTimescale);
        elapsedMs = ((audioPts - basePts_) * 1000) / kTimescale;
    } else {
        const uint64_t now = Platform::NowMs();
        elapsedMs = (int64_t)(now - clockStartMs_) - (int64_t)pausedTotalMs_;
    }

    // Where playback has reached, on the stream's own clock. Audio and video
    // timestamps share it, so they are compared directly rather than through
    // elapsed time.
    int64_t masterPts;
    if (audioUsable) {
        masterPts = audioStart + (int64_t)(audioPlayed * (double)kTimescale);
    } else {
        const uint64_t now = Platform::NowMs();
        const int64_t elapsedMs =
            (int64_t)(now - clockStartMs_) - (int64_t)pausedTotalMs_;
        masterPts = basePts_ + (elapsedMs * kTimescale) / 1000;
    }

    bool haveFrame = false;
    // Take every frame whose time has passed, keeping only the last, so
    // falling behind drops frames rather than playing in slow motion.
    while (!frames_.empty()) {
        if (frames_.front().pts > masterPts) break;

        if (haveFrame) dropped_.fetch_add(1);

        // The caller's previous buffers go back to the pool rather than
        // being freed, so the next frame reuses them.
        // Bound the pool as well. Left alone it grows to the size of the
        // queue, doubling how much decoded video is held.
        if (out.valid() && pool_.size() < 4) pool_.push_back(std::move(out));
        else out = Nv12Frame();
        out = std::move(frames_.front());
        frames_.pop_front();
        haveFrame = true;
    }

    if (haveFrame) {
        lock.unlock();
        spaceCv_.notifyOne();
    }
    return haveFrame;
}

void Player::setPaused(bool paused)
{
    if (paused == paused_.load()) return;

    audio_.setPaused(paused);

    if (paused) {
        pausedAtMs_ = (int64_t)Platform::NowMs();
    } else if (pausedAtMs_ >= 0) {
        // Discount the paused time so the clock does not jump forward.
        pausedTotalMs_ += (uint64_t)((int64_t)Platform::NowMs() - pausedAtMs_);
        pausedAtMs_ = -1;
    }
    paused_ = paused;
}

double Player::positionSeconds() const
{
    if (state_.load() != State::Playing && state_.load() != State::Paused) return 0.0;
    const uint64_t now = (pausedAtMs_ >= 0) ? (uint64_t)pausedAtMs_ : Platform::NowMs();
    const int64_t elapsedMs = (int64_t)(now - clockStartMs_) - (int64_t)pausedTotalMs_;
    return elapsedMs > 0 ? (double)elapsedMs / 1000.0 : 0.0;
}

int Player::queuedFrames()
{
    bj::ScopedLock lock(mutex_);
    return (int)frames_.size();
}

double Player::bufferedAudioSeconds()
{
    bj::ScopedLock lock(audioMutex_);
    return audio_.bufferedSeconds();
}

int Player::bufferedSegments()
{
    bj::ScopedLock lock(segmentMutex_);
    return (int)segments_.size();
}
