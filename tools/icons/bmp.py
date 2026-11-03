# Part of tools/icons: renders the collar and item art out of the game's SWFs to
# PNG, for the collar menu (icons/ beside mgmp.dll). Run:
#   python tools/icons/icons.py <out dir>     (MEWGENICS_GPAK = path to resources.gpak)
import sys as _s, os as _o; _s.path.insert(0, _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), '..'))
import zlib, struct, sys, os
from swf import load, tags
def png(path, w, h, rgba):
    def chunk(t, data): return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    raw = b''.join(b'\0' + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
def bitmaps(b):
    out = {}
    for code, p, ln in tags(b):
        if code in (20, 36):
            cid, fmt, w, h = struct.unpack_from('<HBHH', b, p)
            if fmt != 5: continue
            data = zlib.decompress(b[p+7:p+ln])
            argb = data[:w*h*4]
            rgba = bytearray(len(argb))
            for i in range(0, len(argb), 4):
                a, r, g_, bl = argb[i], argb[i+1], argb[i+2], argb[i+3]
                if code == 36 and a:  # premultiplied
                    r = min(255, r*255//a); g_ = min(255, g_*255//a); bl = min(255, bl*255//a)
                rgba[i:i+4] = bytes((r, g_, bl, a))
            out[cid] = (w, h, bytes(rgba))
    return out
if __name__ == '__main__':
    b = load(sys.argv[1]); outd = sys.argv[2]; os.makedirs(outd, exist_ok=True)
    bm = bitmaps(b)
    for cid, (w, h, rgba) in bm.items():
        png(os.path.join(outd, '%d_%dx%d.png' % (cid, w, h)), w, h, rgba)
    print(len(bm), 'bitmaps')
    print(sorted((w, h, cid) for cid, (w, h, _) in bm.items())[-40:])
