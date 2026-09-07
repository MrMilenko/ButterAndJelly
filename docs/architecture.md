# Architecture

```
src/core        client, playback, HTTP, threading
src/ui          screens, drawing, artwork cache
src/platform    what differs
  wiiu/         hardware H.264, GX2 video
  xenon/        software H.264, XDK filesystem and networking
  desktop/      for testing
```

`src/core` and `src/ui` are shared. Adding a platform means implementing
`core/platform.h`, `core/http.h`, `core/video_decoder.h` and `core/audio_output.h`.

## Playback

A download thread pulls HLS segments. A decode thread demuxes and decodes into
a bounded frame queue, ordered by timestamp because B-frames arrive out of
presentation order. The main thread draws whichever frame is due. Audio runs on
its own thread and its timestamps are the clock.

## Video

| | Wii U | Xbox 360 |
| --- | --- | --- |
| Decode | hardware H.264 | libavcodec 54, software, 4 threads |
| Colour | CPU | GPU, SDL's YUV shader |
| Drawn by | GX2 directly | SDL |
| Cap | 480p | 720p |

The Wii U draws video itself because SDL's renderer there takes RGB only, and
converting on the CPU costs 25ms of a 41.7ms frame. It is drawn after the
interface, since that path leaves GPU state bound that SDL believes is its own.

The Xbox 360 has no H.264 decoder and nothing can transcode into the one format
it does decode, so it decodes in software and hands the planes to SDL. That
needs three SDL2x360 fixes, see [../patches/README.md](../patches/README.md).

## Text and artwork

SDL_ttf on the Wii U and desktop, stb_truetype on the Xbox 360, behind one
interface.

Posters are requested at the size they are drawn, downloaded on a pool thread,
decoded and uploaded a few per frame, and cached on disk.

## Toolchain

The Xbox 360 build goes through [OXDK](https://github.com/MrMilenko/OXDK). Two
of its LLVM patches exist because of this client: variadic doubles and 64 bit
integers each need the single 64 bit register that shares their argument slot.
