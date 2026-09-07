// SPDX-License-Identifier: GPL-2.0-or-later

// MPEG-TS demuxer, for the HLS segments Jellyfin serves.
//
// H.264 inside TS is already Annex-B, which is what the Wii U's hardware
// decoder takes, so nothing rewrites the stream.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// MPEG-TS stream_type values we care about.
enum : uint8_t {
    kStreamTypeMpeg1Audio = 0x03,
    kStreamTypeMpeg2Audio = 0x04,
    kStreamTypeAac        = 0x0F,
    kStreamTypeH264       = 0x1B,
    kStreamTypeAc3        = 0x81,
};

// One complete elementary stream access unit.
struct TsSample {
    std::vector<uint8_t> data;
    int64_t pts = -1;        // 90kHz units, -1 when absent
    int64_t dts = -1;
    bool    video = false;
    uint8_t streamType = 0;
};

class TsDemuxer {
public:
    using SampleFn = std::function<void(const TsSample&)>;

    TsDemuxer();

    // Clears table state and any partly assembled PES packets. Call between
    // titles, and after a seek, since the PIDs may differ.
    void reset();

    // Feeds an arbitrary run of bytes. Does not have to be packet aligned or
    // a whole segment; leftovers are carried to the next call.
    void feed(const uint8_t* data, size_t length, const SampleFn& onSample);

    // Emits whatever is still buffered. A PES packet with no declared length
    // only ends when the next one starts, so the final sample of a stream
    // needs this.
    void flush(const SampleFn& onSample);

    int  videoPid() const { return videoPid_; }
    int  audioPid() const { return audioPid_; }
    uint8_t videoStreamType() const { return videoStreamType_; }
    uint8_t audioStreamType() const { return audioStreamType_; }
    bool tablesReady() const { return videoPid_ >= 0 || audioPid_ >= 0; }

    // Diagnostics.
    uint64_t packetsSeen() const { return packetsSeen_; }
    uint64_t packetsDropped() const { return packetsDropped_; }

private:
    struct PesAssembly {
        std::vector<uint8_t> data;
        bool     started = false;
        uint32_t declaredLength = 0;   // 0 means "until the next start"
    };

    void handlePacket(const uint8_t* packet, const SampleFn& onSample);
    void parsePat(const uint8_t* payload, size_t length);
    void parsePmt(const uint8_t* payload, size_t length);
    void appendPes(PesAssembly& pes, bool unitStart, const uint8_t* payload,
                   size_t length, bool video, const SampleFn& onSample);
    void emit(PesAssembly& pes, bool video, const SampleFn& onSample);

    int     pmtPid_          = -1;
    int     videoPid_        = -1;
    int     audioPid_        = -1;
    uint8_t videoStreamType_ = 0;
    uint8_t audioStreamType_ = 0;

    PesAssembly video_;
    PesAssembly audio_;

    // Bytes left over from the previous feed, shorter than one packet.
    std::vector<uint8_t> partial_;

    uint64_t packetsSeen_    = 0;
    uint64_t packetsDropped_ = 0;
};
