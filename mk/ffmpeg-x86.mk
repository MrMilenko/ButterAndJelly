# The x86 half of libavcodec, for the original Xbox.
#
# Same 54.92 release as the rest of third_party/ffmpeg. The tree arrived from
# XBMC-360 configured --arch=ppc with libavcodec/x86 and libavutil/x86 removed;
# those directories come from xbmc4xbox-redux, which ships the identical
# version for its DVDPlayer. Nothing is mixed between releases.
#
# Only the files providing an entry point the compiled core actually calls,
# rather than the whole directory: the rest reference codecs this build does
# not include. SSE2-only implementations are left out too, since this CPU has
# MMX and SSE and would never reach them.

FFX86_DIR ?= $(FFMPEG_DIR)

FFX86_SRCS := \
    $(FFX86_DIR)/libavcodec/x86/dsputil_mmx.c \
    $(FFX86_DIR)/libavcodec/x86/idct_mmx.c \
    $(FFX86_DIR)/libavcodec/x86/idct_mmx_xvid.c \
    $(FFX86_DIR)/libavcodec/x86/simple_idct.c \
    $(FFX86_DIR)/libavcodec/x86/fdct.c \
    $(FFX86_DIR)/libavcodec/x86/idct_sse2_xvid.c \
    $(FFX86_DIR)/libavcodec/x86/h264dsp_init.c \
    $(FFX86_DIR)/libavcodec/x86/h264_qpel.c \
    $(FFX86_DIR)/libavcodec/x86/h264chroma_init.c \
    $(FFX86_DIR)/libavcodec/x86/h264_intrapred_init.c \
    $(FFX86_DIR)/libavcodec/x86/videodsp_init.c \
    $(FFX86_DIR)/libavcodec/x86/mpegvideo.c \
    $(FFX86_DIR)/libavutil/x86/cpu.c \
    $(FFX86_DIR)/libavutil/x86/float_dsp_init.c

# dsputil_mmx.c is here because dsputil.c tests the plain HAVE_MMX and calls
# ff_dsputil_init_mmx. Leaving it out while the H.264 inits took the assembly
# path left the decoder half accelerated and produced worse pictures than no
# assembly at all. It brings the XviD and Walken IDCTs with it, which is why
# those are listed too.

# The hand written assembly. ffmpeg's own build assembles the whole directory
# and lets the linker drop what nothing references; the ones listed here are
# what the inits above reach, plus the shared constant tables.
FFX86_ASM := \
    $(FFX86_DIR)/libavcodec/x86/dsputil.asm \
    $(FFX86_DIR)/libavcodec/x86/hpeldsp.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_chromamc.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_deblock.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_idct.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_intrapred.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_qpel_8bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_weight.asm \
    $(FFX86_DIR)/libavcodec/x86/videodsp.asm \
    $(FFX86_DIR)/libavcodec/x86/deinterlace.asm \
    $(FFX86_DIR)/libavcodec/x86/mpeg4qpel.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_chromamc_10bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_deblock_10bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_idct_10bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_intrapred_10bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_qpel_10bit.asm \
    $(FFX86_DIR)/libavcodec/x86/h264_weight_10bit.asm \
    $(FFX86_DIR)/libavutil/x86/cpuid.asm \
    $(FFX86_DIR)/libavutil/x86/emms.asm \
    $(FFX86_DIR)/libavutil/x86/float_dsp.asm

# nasm rather than yasm, which ffmpeg was written against. 3.x promotes a
# macro warning the 2013 sources trip constantly, hence the -w- flag.
NASM ?= nasm
FFX86_ASMFLAGS := -f win32 -w-pp-macro-params-legacy \
                  -I$(FFX86_DIR)/libavutil/x86/ \
                  -I$(FFX86_DIR)/libavcodec/x86/ \
                  -I$(FFX86_DIR)/ \
                  -P$(FFX86_DIR)/config.asm
