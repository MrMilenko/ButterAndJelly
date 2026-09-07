// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ts_demux.h"

#include <cstring>

namespace {

constexpr size_t  kPacketSize = 188;
constexpr uint8_t kSyncByte   = 0x47;

// A 33 bit timestamp spread across 5 bytes with marker bits in between.
int64_t ReadTimestamp(const uint8_t* p)
{
    return ((int64_t)(p[0] & 0x0E) << 29) |
           ((int64_t)(p[1])        << 22) |
           ((int64_t)(p[2] & 0xFE) << 14) |
           ((int64_t)(p[3])        <<  7) |
           ((int64_t)(p[4] & 0xFE) >>  1);
}

}  // namespace

TsDemuxer::TsDemuxer()
{
    reset();
}

void TsDemuxer::reset()
{
    pmtPid_          = -1;
    videoPid_        = -1;
    audioPid_        = -1;
    videoStreamType_ = 0;
    audioStreamType_ = 0;
    video_ = PesAssembly();
    audio_ = PesAssembly();
    partial_.clear();
    packetsSeen_    = 0;
    packetsDropped_ = 0;
}

void TsDemuxer::feed(const uint8_t* data, size_t length, const SampleFn& onSample)
{
    if (!data || length == 0) return;

    // Stitch the tail of the last call onto the front of this one.
    if (!partial_.empty()) {
        partial_.insert(partial_.end(), data, data + length);
        data   = partial_.data();
        length = partial_.size();
    }

    size_t offset = 0;
    while (offset + kPacketSize <= length) {
        if (data[offset] != kSyncByte) {
            // Lost alignment. Walk forward to the next plausible sync byte
            // rather than discarding the rest of the buffer.
            size_t scan = offset + 1;
            while (scan < length && data[scan] != kSyncByte) ++scan;
            packetsDropped_ += (scan - offset) / kPacketSize + 1;
            offset = scan;
            continue;
        }

        handlePacket(data + offset, onSample);
        ++packetsSeen_;
        offset += kPacketSize;
    }

    // Keep whatever did not make a whole packet.
    std::vector<uint8_t> leftover(data + offset, data + length);
    partial_.swap(leftover);
}

void TsDemuxer::handlePacket(const uint8_t* packet, const SampleFn& onSample)
{
    const bool unitStart = (packet[1] & 0x40) != 0;
    const int  pid       = ((packet[1] & 0x1F) << 8) | packet[2];
    const uint8_t adaptationControl = (packet[3] >> 4) & 0x03;

    // 0 is reserved, 2 is adaptation field only: neither carries payload.
    if (adaptationControl == 0 || adaptationControl == 2) return;

    size_t payloadOffset = 4;
    if (adaptationControl == 3) {
        const uint8_t adaptationLength = packet[4];
        payloadOffset = 5 + adaptationLength;
        if (payloadOffset >= kPacketSize) return;
    }

    const uint8_t* payload = packet + payloadOffset;
    const size_t   payloadLength = kPacketSize - payloadOffset;

    if (pid == 0) {
        // PAT and PMT are sections: a pointer_field precedes the table when
        // this packet starts one.
        if (!unitStart || payloadLength < 1) return;
        const uint8_t pointer = payload[0];
        if (1 + pointer >= payloadLength) return;
        parsePat(payload + 1 + pointer, payloadLength - 1 - pointer);
        return;
    }

    if (pid == pmtPid_) {
        if (!unitStart || payloadLength < 1) return;
        const uint8_t pointer = payload[0];
        if (1 + pointer >= payloadLength) return;
        parsePmt(payload + 1 + pointer, payloadLength - 1 - pointer);
        return;
    }

    if (pid == videoPid_) {
        appendPes(video_, unitStart, payload, payloadLength, true, onSample);
    } else if (pid == audioPid_) {
        appendPes(audio_, unitStart, payload, payloadLength, false, onSample);
    }
}

void TsDemuxer::parsePat(const uint8_t* section, size_t length)
{
    if (length < 8 || section[0] != 0x00) return;   // table_id 0 is the PAT

    const size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
    if (sectionLength + 3 > length) return;

    // Skip the 8 byte section header; stop before the 4 byte CRC.
    size_t offset = 8;
    const size_t end = 3 + sectionLength - 4;

    while (offset + 4 <= end) {
        const int programNumber = (section[offset] << 8) | section[offset + 1];
        const int pid = ((section[offset + 2] & 0x1F) << 8) | section[offset + 3];
        // Program 0 points at the network information table, not a program.
        if (programNumber != 0) {
            pmtPid_ = pid;
            return;
        }
        offset += 4;
    }
}

void TsDemuxer::parsePmt(const uint8_t* section, size_t length)
{
    if (length < 12 || section[0] != 0x02) return;   // table_id 2 is the PMT

    const size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
    if (sectionLength + 3 > length) return;

    const size_t programInfoLength = ((section[10] & 0x0F) << 8) | section[11];
    size_t offset = 12 + programInfoLength;
    const size_t end = 3 + sectionLength - 4;

    while (offset + 5 <= end) {
        const uint8_t streamType = section[offset];
        const int pid = ((section[offset + 1] & 0x1F) << 8) | section[offset + 2];
        const size_t esInfoLength = ((section[offset + 3] & 0x0F) << 8) | section[offset + 4];

        // Take the first stream of each kind. Jellyfin gives us exactly one
        // of each, having already picked the tracks we asked for.
        if (streamType == kStreamTypeH264 && videoPid_ < 0) {
            videoPid_        = pid;
            videoStreamType_ = streamType;
        } else if ((streamType == kStreamTypeMpeg1Audio ||
                    streamType == kStreamTypeMpeg2Audio ||
                    streamType == kStreamTypeAac ||
                    streamType == kStreamTypeAc3) && audioPid_ < 0) {
            audioPid_        = pid;
            audioStreamType_ = streamType;
        }

        offset += 5 + esInfoLength;
    }
}

void TsDemuxer::appendPes(PesAssembly& pes, bool unitStart, const uint8_t* payload,
                          size_t length, bool video, const SampleFn& onSample)
{
    if (unitStart) {
        // The previous packet ends where this one begins.
        if (pes.started) emit(pes, video, onSample);
        pes.data.clear();
        pes.started = true;
        pes.declaredLength = 0;
    }

    if (!pes.started) return;   // joined mid-packet; wait for a clean start
    pes.data.insert(pes.data.end(), payload, payload + length);

    // Once the declared length is known, emit as soon as it is satisfied.
    // Video PES packets normally declare 0, meaning "until the next start".
    if (pes.declaredLength == 0 && pes.data.size() >= 6) {
        const uint32_t declared = (pes.data[4] << 8) | pes.data[5];
        if (declared > 0) pes.declaredLength = declared + 6;
    }
    if (pes.declaredLength > 0 && pes.data.size() >= pes.declaredLength) {
        emit(pes, video, onSample);
        pes.started = false;
    }
}

void TsDemuxer::emit(PesAssembly& pes, bool video, const SampleFn& onSample)
{
    std::vector<uint8_t>& buffer = pes.data;
    if (buffer.size() < 9) { buffer.clear(); return; }

    // packet_start_code_prefix must be 00 00 01.
    if (buffer[0] != 0x00 || buffer[1] != 0x00 || buffer[2] != 0x01) {
        buffer.clear();
        return;
    }

    const uint8_t flags = buffer[7];
    const uint8_t headerLength = buffer[8];
    const size_t  payloadStart = 9 + headerLength;
    if (payloadStart > buffer.size()) { buffer.clear(); return; }

    TsSample sample;
    sample.video      = video;
    sample.streamType = video ? videoStreamType_ : audioStreamType_;

    const bool hasPts = (flags & 0x80) != 0;
    const bool hasDts = (flags & 0x40) != 0;
    if (hasPts && buffer.size() >= 14) {
        sample.pts = ReadTimestamp(&buffer[9]);
        sample.dts = sample.pts;
        if (hasDts && buffer.size() >= 19) sample.dts = ReadTimestamp(&buffer[14]);
    }

    sample.data.assign(buffer.begin() + payloadStart, buffer.end());
    buffer.clear();

    if (!sample.data.empty()) onSample(sample);
}

void TsDemuxer::flush(const SampleFn& onSample)
{
    if (video_.started) { emit(video_, true, onSample);  video_.started = false; }
    if (audio_.started) { emit(audio_, false, onSample); audio_.started = false; }
}
