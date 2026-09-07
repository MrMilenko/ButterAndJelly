# minimp3

lieff's MP3 decoder, one header, public domain (CC0), vendored unchanged from
https://github.com/lieff/minimp3.

Here because the Xbox 360 has no mpg123 build, and the shape of the library
happens to suit this client better than mpg123's does: `mp3dec_decode_frame`
takes a buffer, decodes one frame, and reports how many bytes it consumed,
which is exactly what feeding an HLS segment in arbitrary pieces needs.

The Wii U and desktop builds keep mpg123, which is packaged for both and has
been tested on hardware.
