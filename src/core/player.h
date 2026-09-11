// SPDX-License-Identifier: GPL-2.0-or-later

// Video playback.
//
// A background thread pulls HLS segments, demuxes them, and decodes into a
// bounded frame queue. The main thread draws whichever frame is due. Audio
// runs on its own thread and its timestamps are the playback clock.

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "core/audio_decoder.h"
#include "core/audio_output.h"
#include "core/jellyfin.h"
#include "core/thread.h"
#include "core/video_frame.h"

class VideoDecoder;

class Player {
public:
    enum class State {
        Idle,
        Opening,     // asking the server how to play this
        Buffering,   // filling the queue before starting the clock
        Playing,
        Paused,
        Ended,
        Failed,
    };

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Starts streaming `itemId`. Returns immediately; watch state().
    // startSeconds picks the segment to begin from. Seeking works by
    // starting at a different point in the same playlist rather than asking
    // the server for a stream that begins elsewhere.
    // Plays a master playlist directly, for services that build their own.
    bool openUrl(const std::string& masterUrl, double startSeconds,
                 std::string& error);

    bool open(JellyfinClient& client, const std::string& itemId,
              int maxHeight, double startSeconds, std::string& error);

    // The same, but naming a version and the tracks within it.
    bool open(JellyfinClient& client, const std::string& itemId,
              int maxHeight, double startSeconds,
              const JellyfinClient::PlaybackRequest& request,
              std::string& error);

    // What the server settled on, once state() has left Opening. Empty
    // sources means the answer has not arrived yet.
    JellyfinClient::PlaybackPlan plan() const;

    // Stops the worker and tells the server to stop transcoding.
    void close();

    State       state() const { return state_.load(); }
    std::string errorText() const;

    // Main thread. Fills `out` with the frame that should be on screen and
    // returns true when it differs from the last one handed over.
    bool nextFrame(Nv12Frame& out);

    void setPaused(bool paused);
    bool paused() const { return paused_.load(); }

    double positionSeconds() const;
    double startOffsetSeconds() const { return startOffsetSeconds_; }
    // How wide the picture should be drawn relative to its height. Zero
    // means the server did not say and the frame's own shape will do.
    double displayAspect() const { return displayAspect_; }
    double durationSeconds() const { return duration_; }

    // Diagnostics for the on-screen overlay.
    int queuedFrames();
    int bufferedSegments();
    double bufferedAudioSeconds();
    bool   audioRunning() const { return audioClockReady_.load(); }
    int decodedFrames() const { return decoded_.load(); }

    // Milliseconds spent inside the decoder, and the frames that covers.
    // Read and reset together by whoever logs them.
    int takeDecodeMs()     { return decodeMs_.exchange(0); }
    int takeDecodeFrames() { return decodeFrames_.exchange(0); }
    int droppedFrames() const { return dropped_.load(); }

private:
    // Downloading and decoding are separate threads with a queue of whole
    // segments between them, so a stalled fetch does not empty the queue.
    void resetForOpen(double startSeconds);
    void downloadUrlThread(std::string masterUrl, double startSeconds);
    void streamFrom(const std::string& masterUrl, double startSeconds);
    void downloadThread(JellyfinClient* client, std::string itemId, int maxHeight,
                        double startSeconds,
                        JellyfinClient::PlaybackRequest request);
    void decodeThread();
    void pushFrame(const Nv12View& view);
    void setFailed(const std::string& message);

    std::atomic<State> state_{ State::Idle };
    std::atomic<bool>  paused_{ false };
    std::atomic<bool>  stopping_{ false };
    std::atomic<int>   decoded_{ 0 };
    std::atomic<int>   decodeMs_{ 0 };
    std::atomic<int>   decodeFrames_{ 0 };
    std::atomic<int>   dropped_{ 0 };
    // Presentation timing depends entirely on frame timestamps being present
    // and ascending. The console's decoder carries them through a double, so
    // whether they survive is worth knowing rather than assuming.
    std::atomic<int>   ptsMissing_{ 0 };
    std::atomic<int>   ptsBackwards_{ 0 };
    std::atomic<int>   reordered_{ 0 };
    int64_t            lastSeenPts_ = INT64_MIN;
    int                ptsLogged_   = 0;

    bj::Thread downloader_;
    bj::Thread decoder_;

    // Downloaded but not yet decoded. A few segments is tens of seconds of
    // video, which is what lets a slow fetch pass unnoticed.
    bj::Mutex                        segmentMutex_;
    bj::CondVar           segmentReadyCv_;
    bj::CondVar           segmentSpaceCv_;
    std::deque<std::vector<uint8_t>>  segments_;
    bool                              downloadComplete_ = false;

    mutable bj::Mutex     mutex_;
    bj::CondVar spaceCv_;
    std::deque<Nv12Frame>  frames_;
    // Retired frames, kept for their allocations rather than freed. After
    // the first few frames the pipeline stops calling the allocator at all.
    std::vector<Nv12Frame> pool_;
    std::string            error_;
    std::string            playSessionId_;
    JellyfinClient::PlaybackPlan plan_;

    double  duration_ = 0.0;
    // Worked out from the first frame's size, since it depends on what the
    // server sent rather than what was asked for.
    size_t  queueLimit_ = 0;
    size_t  framesBeforeStart_ = 8;
    // Where in the film this stream begins, so reported progress and the
    // on-screen position are absolute rather than relative to the request.
    double  startOffsetSeconds_ = 0.0;
    double  displayAspect_ = 0.0;
    int64_t basePts_  = -1;      // PTS of the first frame, 90kHz

    // Wall clock reference for presentation, in milliseconds.
    uint64_t clockStartMs_ = 0;
    int64_t  pausedAtMs_   = -1;
    uint64_t pausedTotalMs_ = 0;

    // Audio doubles as the clock. A wall clock drifts against the sound
    // card, which shows up as lip sync error after a couple of minutes;
    // asking the device what it has actually played cannot drift from what
    // the viewer hears.
    Mp3Decoder  audioDecoder_;
    AudioOutput audio_;
    bj::Mutex  audioMutex_;
    // Guarded by audioMutex_ rather than atomic: this is a 32-bit PowerPC
    // and it has no 64-bit atomics, so std::atomic<int64_t> does not link.
    // The companion flag is a plain bool atomic, which does.
    int64_t              audioStartPts_ = -1;
    std::atomic<bool>    audioClockReady_{ false };
    bool                 tablesReported_ = false;

    JellyfinClient* client_ = nullptr;
};
