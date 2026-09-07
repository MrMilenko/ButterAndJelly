// SPDX-License-Identifier: GPL-2.0-or-later

// audio_output.h: PCM to the speakers, and the clock everything syncs to.
//
// Audio is the master clock during playback. Video timing previously ran off
// the wall clock, which drifts against the sound card and shows up as lip
// sync error over a couple of minutes. Asking the audio device how much of
// what we queued has actually been played gives a clock that cannot drift
// from what the viewer hears.

#pragma once

#include <cstdint>
#include <string>

class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool open(int sampleRate, int channels, std::string& error);
    void close();
    bool isOpen() const { return deviceId_ != 0; }

    // Hands PCM to the device. Queued audio is played in order and the
    // device buffers as much as it is given.
    void queue(const uint8_t* pcm, size_t bytes);

    // Seconds of audio actually played, from what the device has consumed
    // rather than from a timer.
    double playedSeconds() const;

    // Seconds still queued and not yet heard. Below about a tenth of a
    // second, playback is about to run dry.
    double bufferedSeconds() const;

    void setPaused(bool paused);
    void flush();

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

private:
    uint32_t deviceId_ = 0;
    int      sampleRate_ = 0;
    int      channels_   = 0;
    int      bytesPerFrame_ = 0;
    uint64_t queuedBytes_ = 0;   // everything ever handed over
};
