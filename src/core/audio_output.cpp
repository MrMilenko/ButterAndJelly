// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/audio_output.h"

#include "core/log.h"

#include <SDL.h>

AudioOutput::~AudioOutput()
{
    close();
}

bool AudioOutput::open(int sampleRate, int channels, std::string& error)
{
    close();

    if (sampleRate <= 0 || channels <= 0) { error = "Bad audio format"; return false; }

    SDL_AudioSpec want{};
    want.freq     = sampleRate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    // Large enough that a late refill does not click, small enough that
    // pausing responds promptly.
    want.samples  = 2048;
    want.callback = nullptr;   // queue driven rather than callback driven

    SDL_AudioSpec have{};
    const SDL_AudioDeviceID device =
        SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (device == 0) {
        error = std::string("Could not open the audio device: ") + SDL_GetError();
        return false;
    }

    deviceId_       = device;
    sampleRate_     = have.freq;
    channels_       = have.channels;
    bytesPerFrame_  = (int)(SDL_AUDIO_BITSIZE(have.format) / 8) * have.channels;
    queuedBytes_    = 0;

    // Opened paused. Audio decodes ahead of video during pre-roll, and
    // letting it play immediately puts the clock half a second in front of
    // the first frame before playback even starts.
    SDL_PauseAudioDevice(device, 1);
    LOGF("[audio] device open (paused): %d Hz, %d channel(s), %d byte frames",
         sampleRate_, channels_, bytesPerFrame_);
    return true;
}

void AudioOutput::close()
{
    if (deviceId_ == 0) return;
    SDL_CloseAudioDevice((SDL_AudioDeviceID)deviceId_);
    deviceId_ = 0;
    sampleRate_ = channels_ = bytesPerFrame_ = 0;
    queuedBytes_ = 0;
}

void AudioOutput::queue(const uint8_t* pcm, size_t bytes)
{
    if (deviceId_ == 0 || !pcm || bytes == 0) return;
    if (SDL_QueueAudio((SDL_AudioDeviceID)deviceId_, pcm, (Uint32)bytes) == 0) {
        queuedBytes_ += bytes;
    }
}

double AudioOutput::playedSeconds() const
{
    if (deviceId_ == 0 || bytesPerFrame_ <= 0 || sampleRate_ <= 0) return 0.0;

    const Uint32 pending = SDL_GetQueuedAudioSize((SDL_AudioDeviceID)deviceId_);
    if (queuedBytes_ < pending) return 0.0;

    const uint64_t played = queuedBytes_ - pending;
    return (double)played / (double)(bytesPerFrame_ * sampleRate_);
}

double AudioOutput::bufferedSeconds() const
{
    if (deviceId_ == 0 || bytesPerFrame_ <= 0 || sampleRate_ <= 0) return 0.0;
    const Uint32 pending = SDL_GetQueuedAudioSize((SDL_AudioDeviceID)deviceId_);
    return (double)pending / (double)(bytesPerFrame_ * sampleRate_);
}

void AudioOutput::setPaused(bool paused)
{
    if (deviceId_ == 0) return;
    SDL_PauseAudioDevice((SDL_AudioDeviceID)deviceId_, paused ? 1 : 0);
}

void AudioOutput::flush()
{
    if (deviceId_ == 0) return;
    SDL_ClearQueuedAudio((SDL_AudioDeviceID)deviceId_);
    queuedBytes_ = 0;
}
