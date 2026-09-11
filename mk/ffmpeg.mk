# FFmpeg 1.1 for the Xbox 360, from XBMC-360's hand-adapted tree. The XDK
# decodes XMV and nothing transcodes into it, so H.264 is decoded in software.
# This is the subset that needs, and no more.
#
# That tree arrived with trailing commas on 32 preprocessor directives, and
# third_party/xdkcompat/inttypes.h stands in for what the 2005 CRT lacks.
# Threading is off; see the top of third_party/ffmpeg/config.h.

FFMPEG_DIR ?= third_party/ffmpeg

FFMPEG_SRCS := $(wildcard $(FFMPEG_DIR)/libavcodec/*.c) \
               $(wildcard $(FFMPEG_DIR)/libavutil/*.c)
# The template files are #included by their siblings, never compiled alone.
FFMPEG_SRCS := $(filter-out %_template.c,$(FFMPEG_SRCS))

# HAVE_AV_CONFIG_H says "this file is part of FFmpeg". Anything that only uses
# the library must not define it: the internal headers are not C++ safe.
#
# libavutil/internal.h is force-included too. libavcodec has one of its own, so
# a file in that directory asking for "internal.h" gets its neighbour and never
# sees INT_BIT. Upstream relies on include order; this says it outright.
#
# -fno-strict-aliasing because this tree type-puns freely.
# ffmpeg is cdecl throughout while the rest of the program is stdcall. Only
# VideoDecoder::Create crosses back, and it declares the convention itself.
ifeq ($(OXDK_TARGET),xbox)
FFMPEG_ABI_FLAGS := -Xclang -fdefault-calling-conv=cdecl
endif

# -fgnu89-inline: av_extern_inline is a plain inline, which emits nothing
# under C99.
#
# See xdk_static_inline.h for what that switch does to the XDK headers.
FFMPEG_BUILD_FLAGS := -DHAVE_AV_CONFIG_H -fno-strict-aliasing -fgnu89-inline \
                      -include third_party/xdkcompat/xdk_static_inline.h \
                      $(FFMPEG_ABI_FLAGS) \
                      -I$(FFMPEG_DIR) -Ithird_party/xdkcompat \
                      -include $(FFMPEG_DIR)/libavutil/intmath.h \
                      -include $(FFMPEG_DIR)/libavutil/internal.h \
                      -include $(FFMPEG_DIR)/config.h

# What a consumer needs: the public headers, and nothing else.
FFMPEG_USE_FLAGS := -I$(FFMPEG_DIR) -Ithird_party/xdkcompat
