// SPDX-License-Identifier: GPL-2.0-or-later

// What this build offers. macOS is the whole client; a console turns off
// only what its hardware cannot do.

#pragma once

// Which audio codecs to ask the server for, in preference order. The
// original Xbox spends its budget on video, so MP3 through minimp3.
#if defined(_XBOX) && !defined(_XENON)
  #define BJ_AUDIO_CODECS "mp3"
  #define BJ_AUDIO_AAC 0
#elif defined(__WIIU__)
  // mpg123 only; devkitPro ships no AAC decoder for this target.
  #define BJ_AUDIO_CODECS "mp3"
  #define BJ_AUDIO_AAC 0
#elif defined(_XENON)
  // libavcodec 54: decode_audio4 rather than send_packet, and no
  // AVChannelLayout, so AAC needs a decoder written against that API.
  #define BJ_AUDIO_CODECS "mp3"
  #define BJ_AUDIO_AAC 0
#else
  #define BJ_AUDIO_CODECS "aac,mp3"
  #define BJ_AUDIO_AAC 1
#endif

inline constexpr const char* kAudioCodecs = BJ_AUDIO_CODECS;

// The player only demuxes MPEG-TS; anything else goes through the server.
#define BJ_DIRECT_PLAY_CONTAINERS "ts"
#define BJ_VIDEO_CODECS           "h264"

// The original Xbox is targeted at a stock 64MB machine, not an upgraded one.
// Roughly: 8MB of decoded frames, 2MB of artwork, and what the video textures
// and the XDK take, which leaves room on the machine most people have.

#if defined(_XBOX) && !defined(_XENON)
  // 733MHz Pentium III, 64MB shared with the GPU.
  #define BJ_MAX_VIDEO_HEIGHT   480
  // Without a width cap a wide source comes back as 884x480.
  #define BJ_MAX_VIDEO_WIDTH    640
  #define BJ_MAX_FRAMERATE      30
  #define BJ_SUBTITLES          1
  #define BJ_CAST_ARTWORK       0
  #define BJ_CHAPTER_ARTWORK    0
  #define BJ_TRICKPLAY          0
  #define BJ_MAX_GRID_COLUMNS   4
  #define BJ_REQUEST_LIST       12
  // 154 pads to a 256x256 texture. 342 would pad to 512x1024, which is 2MB
  // of a 64MB machine for one poster.
  #define BJ_POSTER_WIDTH       154
  // 64MB shared with the GPU. A tile is drawn at 207x310 on a 720p output,
  // which pads to a 512KB texture; capped here it is 128KB.
  #define BJ_ART_MAX_WIDTH      128
  #define BJ_ART_TEXTURES       16
  #define BJ_ART_IN_FLIGHT      2
  #define BJ_FRAME_QUEUE_MB     8
#elif defined(_XENON)
  #define BJ_MAX_VIDEO_HEIGHT   720
  #define BJ_MAX_VIDEO_WIDTH    0
  #define BJ_MAX_FRAMERATE      0
  #define BJ_SUBTITLES          1
  #define BJ_CAST_ARTWORK       1
  #define BJ_CHAPTER_ARTWORK    0
  #define BJ_TRICKPLAY          0
  #define BJ_MAX_GRID_COLUMNS   5
  #define BJ_REQUEST_LIST       20
  #define BJ_POSTER_WIDTH       342
  #define BJ_ART_MAX_WIDTH      0
  #define BJ_ART_TEXTURES       96
  #define BJ_ART_IN_FLIGHT      3
  #define BJ_FRAME_QUEUE_MB     20
#elif defined(__WIIU__)
  #define BJ_MAX_VIDEO_HEIGHT   720
  #define BJ_MAX_VIDEO_WIDTH    0
  #define BJ_MAX_FRAMERATE      0
  #define BJ_SUBTITLES          1
  #define BJ_CAST_ARTWORK       1
  #define BJ_CHAPTER_ARTWORK    0
  #define BJ_TRICKPLAY          0
  #define BJ_MAX_GRID_COLUMNS   5
  #define BJ_REQUEST_LIST       20
  #define BJ_POSTER_WIDTH       342
  #define BJ_ART_MAX_WIDTH      0
  #define BJ_ART_TEXTURES       96
  #define BJ_ART_IN_FLIGHT      3
  #define BJ_FRAME_QUEUE_MB     20
#else
  #define BJ_MAX_VIDEO_HEIGHT   1080
  #define BJ_MAX_VIDEO_WIDTH    0
  #define BJ_MAX_FRAMERATE      0
  #define BJ_SUBTITLES          1
  #define BJ_CAST_ARTWORK       1
  #define BJ_CHAPTER_ARTWORK    1
  #define BJ_TRICKPLAY          1
  #define BJ_MAX_GRID_COLUMNS   8
  #define BJ_REQUEST_LIST       30
  #define BJ_POSTER_WIDTH       500
  #define BJ_ART_MAX_WIDTH      0
  #define BJ_ART_TEXTURES       512
  #define BJ_ART_IN_FLIGHT      4
  #define BJ_FRAME_QUEUE_MB     20
#endif

// A request carries only an id, so every entry costs a lookup.

// Formats the server can hand over as a file rather than burn in.
#define BJ_SUBTITLE_FORMATS "vtt,srt,subrip,ass,ssa"
