# stb

Sean Barrett's single-file public domain libraries, vendored unchanged.

- `stb_image.h` v2.30 -- JPEG and PNG decoding, in place of SDL_image
- `stb_truetype.h` v1.26 -- TrueType glyph rasterising, in place of SDL_ttf/FreeType
- `stb_image_write.h` -- PNG encoding, for the screenshot path SDL_image would do

Both are here because the Xbox 360 has no build of the library each replaces,
and porting libpng, libjpeg and FreeType to a console with no filesystem
conventions and a 2006 CRT is a great deal of work for something two headers
already do. They are compiled into the 360 build only; the Wii U and desktop
builds keep SDL_image and SDL_ttf, which work and have been tested on hardware.
