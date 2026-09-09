; Hand written, like config.h beside it. ffmpeg's configure normally emits
; this; we are not running it.
;
; The Pentium III has MMX, MMXEXT and SSE. Later sets are still assembled
; because ffmpeg dispatches on cpuid at run time and simply never calls them
; on this machine. AVX and friends are left out to keep the image down.
%define ARCH_X86_32 1
%define ARCH_X86_64 0
%define PIC 0
%define HAVE_ALIGNED_STACK 0
%define HAVE_CPUNOP 0
%define private_prefix ff

; i386 Windows decorates C symbols with a leading underscore, and x86inc.asm
; keys its mangle() on this being defined. Without it the assembly asks the
; linker for ff_h264_cabac_tables while C exported _ff_h264_cabac_tables.
%define PREFIX

%define HAVE_MMX_EXTERNAL 1
%define HAVE_MMXEXT_EXTERNAL 1
%define HAVE_SSE_EXTERNAL 1
%define HAVE_SSE2_EXTERNAL 0
%define HAVE_SSSE3_EXTERNAL 0
%define HAVE_SSE4_EXTERNAL 0
%define HAVE_AMD3DNOW_EXTERNAL 0
%define HAVE_AMD3DNOWEXT_EXTERNAL 0

%define HAVE_AVX_EXTERNAL 0
%define HAVE_AVX2_EXTERNAL 0
%define HAVE_XOP_EXTERNAL 0
%define HAVE_FMA3_EXTERNAL 0
%define HAVE_FMA4_EXTERNAL 0
