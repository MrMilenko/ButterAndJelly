// SPDX-License-Identifier: GPL-2.0-or-later

// Draws decoded video with the GPU rather than the CPU.
//
// SDL's Wii U renderer takes RGB only, so going through it costs a colour
// conversion and an upload, about 25ms of a 41.7ms frame. This bypasses SDL
// for the video image alone: the NV12 planes become textures and a shader
// converts while sampling. The interface still draws over the top.

#pragma once

#include <string>

#include "core/video_frame.h"

class Gx2Video {
public:
    Gx2Video() = default;
    ~Gx2Video();

    Gx2Video(const Gx2Video&) = delete;
    Gx2Video& operator=(const Gx2Video&) = delete;

    // Loads the compiled shader from `shaderPath` and builds the fixed GPU
    // state. Failure leaves the object unusable but safe.
    bool init(const std::string& shaderPath, std::string& error);
    void shutdown();
    bool ready() const { return ready_; }

    // Draws `frame` into the rectangle given in normalised device
    // coordinates, where -1,-1 is bottom left and 1,1 is top right.
    void draw(const Nv12Frame& frame, float left, float top,
              float right, float bottom);

    // Milliseconds spent in the last draw, split so it is clear which part
    // costs what rather than having to guess.
    double lastCopyMs() const { return copyMs_; }
    double lastFlushMs() const { return flushMs_; }
    double lastDrawMs() const { return drawMs_; }

private:
    bool loadShaders(const std::string& path, std::string& error);
    bool ensurePlaneBuffers(const Nv12Frame& frame);

    struct Impl;
    Impl* impl_ = nullptr;
    bool  ready_ = false;
    double copyMs_ = 0.0, flushMs_ = 0.0, drawMs_ = 0.0;
};
