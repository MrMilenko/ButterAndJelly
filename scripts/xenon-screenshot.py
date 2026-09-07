# Grab the front buffer and untile it with the real Xenos address formula.
#
# The OXDK360 tool's decoder is a fixed permutation of the ten address bits
# inside a 32x32 tile. XGAddress2DTiledOffset is not a permutation: its last
# term mixes x and y together, so no fixed mapping can reproduce it, and what
# comes out is a mostly-correct image with blocks exchanged in a checkerboard.
import re, socket, struct, sys, zlib

HOST = sys.argv[1]
OUT  = sys.argv[2]

def xg_address_2d_tiled_offset(x, y, width, texel_pitch):
    aligned_width = (width + 31) & ~31
    log_bpp = (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2))
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) \
             + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return ((((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63)
             + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp)

s = socket.create_connection((HOST, 730), timeout=30)
f = s.makefile('rwb')
f.readline()
f.write(b'screenshot\r\n'); f.flush()
reply = f.readline().decode(errors='replace').strip()
if not reply.startswith('203'):
    sys.exit('screenshot refused: ' + reply)
meta = f.readline().decode(errors='replace').strip()
d = {k: int(v, 16) for k, v in re.findall(r'(\w+)=(0x[0-9a-fA-F]+)', meta)}
w, h, total = d['width'], d['height'], d['framebuffersize']
print(meta)
buf = b''
while len(buf) < total:
    c = f.read(min(65536, total - len(buf)))
    if not c: break
    buf += c
print('got', len(buf), 'of', total)

rows = []
ntex = len(buf) // 4
for y in range(h):
    row = bytearray(w * 3)
    for x in range(w):
        t = xg_address_2d_tiled_offset(x, y, w, 4)
        if t >= ntex: continue
        o = t * 4
        b_, g_, r_ = buf[o], buf[o+1], buf[o+2]
        row[x*3], row[x*3+1], row[x*3+2] = r_, g_, b_
    rows.append(bytes(row))

raw = b''.join(b'\x00' + r for r in rows)
def chunk(tag, data):
    return (struct.pack('>I', len(data)) + tag + data
            + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
png = (b'\x89PNG\r\n\x1a\n'
       + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(raw, 6))
       + chunk(b'IEND', b''))
open(OUT, 'wb').write(png)
print('wrote', OUT, w, 'x', h)
