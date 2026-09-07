# FFmpeg 1.1 for the Xbox 360, from XBMC-360's hand-adapted tree.
#
# There is no H.264 in the XDK: its decoder is XMV, which is WMV9 and VC-1,
# and no encoder exists to transcode into that, so H.264 is decoded in software.
# XBMC-360 carries an FFmpeg already adapted for this XDK: big-endian, with
# _XBOX branches through the configuration. This is the subset an H.264
# decoder needs, and no more.
#
# Two things had to be repaired to build it, both damage in that tree rather
# than anything about this toolchain:
#
#   * 32 preprocessor directives across two files carry trailing commas --
#     "#if FF_API_MPV_GLOBAL_OPTS,,,,", which no compiler accepts.
#   * third_party/xdkcompat/inttypes.h supplies what the XDK's 2005 CRT does
#     not, which is the whole header.
#
# Threading is off; see the note at the top of third_party/ffmpeg/config.h.

FFMPEG_DIR ?= third_party/ffmpeg

FFMPEG_SRCS := $(wildcard $(FFMPEG_DIR)/libavcodec/*.c) \
               $(wildcard $(FFMPEG_DIR)/libavutil/*.c)
# The template files are #included by their siblings, never compiled alone.
FFMPEG_SRCS := $(filter-out %_template.c,$(FFMPEG_SRCS))

# FFmpeg builds itself with its own configuration visible and its own headers
# first; HAVE_AV_CONFIG_H is what says "this translation unit is part of
# FFmpeg". Anything that merely *uses* the library must not define it, or it
# gets the internal headers, which are not C++ safe.
# libavutil/internal.h is force-included as well as the two the vcproj names.
# libavcodec has an internal.h of its own, and a file in that directory asking
# for "internal.h" gets its neighbour rather than libavutil's, so INT_BIT and
# friends never arrive. Upstream avoids it by include order; this is the same
# thing said explicitly.
FFMPEG_BUILD_FLAGS := -DHAVE_AV_CONFIG_H -I$(FFMPEG_DIR) -Ithird_party/xdkcompat \
                      -include $(FFMPEG_DIR)/libavutil/intmath.h \
                      -include $(FFMPEG_DIR)/libavutil/internal.h \
                      -include $(FFMPEG_DIR)/config.h

# What a consumer needs: the public headers, and nothing else.
FFMPEG_USE_FLAGS := -I$(FFMPEG_DIR) -Ithird_party/xdkcompat
