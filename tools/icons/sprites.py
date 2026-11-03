# Part of tools/icons: renders the collar and item art out of the game's SWFs to
# PNG, for the collar menu (icons/ beside mgmp.dll). Run:
#   python tools/icons/icons.py <out dir>     (MEWGENICS_GPAK = path to resources.gpak)
import sys as _s, os as _o; _s.path.insert(0, _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), '..'))
import struct, sys, pickle, os
from swf import load, tags, symbols
# cache the decompressed swf
def cached(name):
    c = 'cache_' + name.replace('/', '_') + '.bin'
    if os.path.exists(c): return open(c, 'rb').read()
    b = load(name); open(c, 'wb').write(b); return b
def chars(b):
    """id -> (code, payload offset, len) for every definition tag"""
    out = {}
    for code, p, ln in tags(b):
        if code in (2, 22, 32, 83, 39, 36, 20, 21, 35, 6, 37, 11, 33, 10, 48, 75, 46, 84):
            cid = struct.unpack_from('<H', b, p)[0]
            out[cid] = (code, p, ln)
    return out
def subtags(b, p, ln):
    q = p + 4; end = p + ln
    while q < end:
        h = struct.unpack_from('<H', b, q)[0]; code = h >> 6; l = h & 63; q += 2
        if l == 63: l = struct.unpack_from('<I', b, q)[0]; q += 4
        yield code, q, l
        q += l
        if code == 0: break
if __name__ == '__main__':
    b = cached(sys.argv[1]); ch = chars(b); sym = {v: k for k, v in symbols(b).items()}
    for name in sys.argv[2:]:
        cid = sym[name]; code, p, ln = ch[cid]
        nframes = struct.unpack_from('<H', b, p + 2)[0]
        print(name, cid, 'frames', nframes)
        f = 0; places = 0
        for c, q, l in subtags(b, p, ln):
            if c == 1: f += 1
            elif c == 43: print('  label frame', f, b[q:b.index(b'\0', q)].decode('latin1'))
            elif c in (26, 70):
                flags = b[q]; depth = struct.unpack_from('<H', b, q + 1 + (1 if c == 70 else 0))[0]
                off = q + 3 + (1 if c == 70 else 0)
                chid = struct.unpack_from('<H', b, off)[0] if flags & 2 else None
                if f < 3 or places < 40: print('  f%d place depth %d char %s (%s)' % (f, depth, chid, ch.get(chid, ('?',))[0] if chid else '-'))
                places += 1
        print('  places', places)
