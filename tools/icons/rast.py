# Part of tools/icons: renders the collar and item art out of the game's SWFs to
# PNG, for the collar menu (icons/ beside mgmp.dll). Run:
#   python tools/icons/icons.py <out dir>     (MEWGENICS_GPAK = path to resources.gpak)
import sys as _s, os as _o; _s.path.insert(0, _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), '..'))
# A small SWF renderer: shapes (DefineShape1-4) and sprites (display lists),
# solid / gradient / bitmap fills, strokes, matrices, colour transforms.
# Renders one character's frame to an RGBA PNG, supersampled for anti-aliasing.
import struct, zlib, math, sys, os
from sprites import cached, chars, subtags
from bmp import png


class Bits:
    def __init__(self, b, p):
        self.b = b; self.p = p; self.bit = 0
    def align(self):
        if self.bit: self.p += 1; self.bit = 0
    def ub(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p] >> (7 - self.bit)) & 1)
            self.bit += 1
            if self.bit == 8: self.bit = 0; self.p += 1
        return v
    def sb(self, n):
        if n == 0: return 0
        v = self.ub(n)
        if v & (1 << (n - 1)): v -= 1 << n
        return v
    def fb(self, n): return self.sb(n) / 65536.0
    def u8(self):
        self.align(); v = self.b[self.p]; self.p += 1; return v
    def u16(self):
        self.align(); v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v
    def s16(self):
        self.align(); v = struct.unpack_from('<h', self.b, self.p)[0]; self.p += 2; return v
    def rect(self):
        self.align(); n = self.ub(5)
        r = [self.sb(n) for _ in range(4)]; self.align(); return r
    def matrix(self):
        self.align()
        sx = sy = 1.0; r0 = r1 = 0.0
        if self.ub(1):
            n = self.ub(5); sx = self.fb(n); sy = self.fb(n)
        if self.ub(1):
            n = self.ub(5); r0 = self.fb(n); r1 = self.fb(n)
        n = self.ub(5); tx = self.sb(n); ty = self.sb(n)
        self.align()
        return (sx, r0, r1, sy, tx, ty)
    def cxform(self, alpha=True):
        self.align()
        has_add = self.ub(1); has_mult = self.ub(1); n = self.ub(4)
        m = [256, 256, 256, 256]; a = [0, 0, 0, 0]
        k = 4 if alpha else 3
        if has_mult: m[:k] = [self.sb(n) for _ in range(k)]
        if has_add: a[:k] = [self.sb(n) for _ in range(k)]
        self.align()
        return (m, a)
    def color(self, alpha):
        self.align()
        r, g, b = self.b[self.p:self.p + 3]; self.p += 3
        a = 255
        if alpha: a = self.b[self.p]; self.p += 1
        return (r, g, b, a)
    def string(self):
        self.align(); e = self.b.index(b'\0', self.p); s = self.b[self.p:e]; self.p = e + 1; return s


def mat_mul(a, b):
    """a after b: x -> a(b(x))"""
    sx = a[0] * b[0] + a[2] * b[1]
    r0 = a[1] * b[0] + a[3] * b[1]
    r1 = a[0] * b[2] + a[2] * b[3]
    sy = a[1] * b[2] + a[3] * b[3]
    tx = a[0] * b[4] + a[2] * b[5] + a[4]
    ty = a[1] * b[4] + a[3] * b[5] + a[5]
    return (sx, r0, r1, sy, tx, ty)


def apply(m, x, y): return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


def invert(m):
    det = m[0] * m[3] - m[1] * m[2]
    if abs(det) < 1e-12: return None
    i = (m[3] / det, -m[1] / det, -m[2] / det, m[0] / det)
    tx = -(i[0] * m[4] + i[2] * m[5]); ty = -(i[1] * m[4] + i[3] * m[5])
    return (i[0], i[1], i[2], i[3], tx, ty)


IDENT = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)
NOCX = ([256, 256, 256, 256], [0, 0, 0, 0])


def cx_mul(outer, inner):
    m = [inner[0][i] * outer[0][i] // 256 for i in range(4)]
    a = [inner[1][i] * outer[0][i] // 256 + outer[1][i] for i in range(4)]
    return (m, a)


def cx_apply(cx, c):
    return tuple(max(0, min(255, c[i] * cx[0][i] // 256 + cx[1][i])) for i in range(4))


# --- shapes ---------------------------------------------------------------------

def read_fill(bs, version):
    t = bs.u8()
    if t == 0x00:
        return ('solid', bs.color(version >= 3))
    if t in (0x10, 0x12, 0x13):
        m = bs.matrix()
        bs.align()
        spread = bs.ub(2); interp = bs.ub(2); n = bs.ub(4)
        stops = []
        for _ in range(n):
            r = bs.u8(); c = bs.color(version >= 3); stops.append((r, c))
        focal = 0.0
        if t == 0x13: focal = bs.s16() / 256.0
        return ('linear' if t == 0x10 else 'radial', m, stops)
    if t in (0x40, 0x41, 0x42, 0x43):
        bid = bs.u16(); m = bs.matrix()
        return ('bitmap', bid, m, t in (0x40, 0x42))
    raise Exception('fill type %x' % t)


def read_styles(bs, version):
    n = bs.u8()
    if n == 0xFF and version >= 2: n = bs.u16()
    fills = [read_fill(bs, version) for _ in range(n)]
    n = bs.u8()
    if n == 0xFF and version >= 2: n = bs.u16()
    lines = []
    for _ in range(n):
        w = bs.u16()
        if version == 4:
            bs.align()
            start_cap = bs.ub(2); join = bs.ub(2); has_fill = bs.ub(1)
            bs.ub(1); bs.ub(1); bs.ub(1); bs.ub(5); bs.ub(1); bs.ub(2)
            if join == 2: bs.u16()
            if has_fill:
                f = read_fill(bs, version)
                col = f[1] if f[0] == 'solid' else (f[2][0][1] if f[0] in ('linear', 'radial') else (0, 0, 0, 255))
            else:
                col = bs.color(True)
        else:
            col = bs.color(version >= 3)
        lines.append((w, col))
    bs.align()
    nf = bs.ub(4); nl = bs.ub(4)
    return fills, lines, nf, nl


def flatten_quad(x0, y0, cx, cy, x1, y1, out):
    d = abs(cx - (x0 + x1) / 2) + abs(cy - (y0 + y1) / 2)
    n = max(2, min(24, int(math.sqrt(d) / 3) + 2))
    for i in range(1, n + 1):
        t = i / n; u = 1 - t
        out.append((u * u * x0 + 2 * u * t * cx + t * t * x1, u * u * y0 + 2 * u * t * cy + t * t * y1))


def parse_shape(b, code, p, ln):
    version = {2: 1, 22: 2, 32: 3, 83: 4}[code]
    bs = Bits(b, p)
    cid = bs.u16(); bs.rect()
    if version == 4: bs.rect(); bs.u8()
    fills, lines, nf, nl = read_styles(bs, version)
    # Each group: (fills, lines, [segments as (style0, style1, line, points)])
    groups = [(fills, lines, [])]
    x = y = 0
    f0 = f1 = ln_ = 0
    cur = None
    def start():
        nonlocal cur
        cur = [f0, f1, ln_, [(x, y)]]
        groups[-1][2].append(cur)
    start()
    while True:
        if bs.ub(1) == 0:
            flags = bs.ub(5)
            if flags == 0: break
            if flags & 1:
                n = bs.ub(5); x = bs.sb(n); y = bs.sb(n)
            if flags & 2: f0 = bs.ub(nf)
            if flags & 4: f1 = bs.ub(nf)
            if flags & 8: ln_ = bs.ub(nl)
            if flags & 16:
                fills, lines, nf, nl = read_styles(bs, version)
                groups.append((fills, lines, []))
                f0 = f1 = ln_ = 0
            start()
        else:
            if bs.ub(1):  # straight
                n = bs.ub(4) + 2
                if bs.ub(1):
                    dx = bs.sb(n); dy = bs.sb(n)
                else:
                    if bs.ub(1): dx = 0; dy = bs.sb(n)
                    else: dx = bs.sb(n); dy = 0
                x += dx; y += dy
                cur[3].append((x, y))
            else:
                n = bs.ub(4) + 2
                cdx = bs.sb(n); cdy = bs.sb(n); adx = bs.sb(n); ady = bs.sb(n)
                cx = x + cdx; cy = y + cdy; x = cx + adx; y = cy + ady
                flatten_quad(cur[3][-1][0], cur[3][-1][1], cx, cy, x, y, cur[3])
    return groups


# --- the canvas -------------------------------------------------------------------

class Canvas:
    def __init__(self, w, h):
        self.w = w; self.h = h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]   # premultiplied floats

    def blend(self, i, c):
        a = c[3] / 255.0
        if a <= 0: return
        d = self.px[i]
        k = 1 - a
        d[0] = c[0] / 255.0 * a + d[0] * k
        d[1] = c[1] / 255.0 * a + d[1] * k
        d[2] = c[2] / 255.0 * a + d[2] * k
        d[3] = a + d[3] * k

    def fill_polys(self, polys, paint):
        """Even-odd scanline fill of closed polylines (canvas pixels)."""
        edges = []
        for pts in polys:
            for i in range(len(pts) - 1):
                (x0, y0), (x1, y1) = pts[i], pts[i + 1]
                if y0 == y1: continue
                edges.append((x0, y0, x1, y1))
        if not edges: return
        ymin = max(0, int(min(min(e[1], e[3]) for e in edges)))
        ymax = min(self.h - 1, int(max(max(e[1], e[3]) for e in edges)) + 1)
        for yy in range(ymin, ymax + 1):
            sy = yy + 0.5
            xs = []
            for x0, y0, x1, y1 in edges:
                if (y0 <= sy < y1) or (y1 <= sy < y0):
                    xs.append(x0 + (sy - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for k in range(0, len(xs) - 1, 2):
                a = max(0, int(math.ceil(xs[k] - 0.5))); e = min(self.w - 1, int(math.floor(xs[k + 1] - 0.5)))
                row = yy * self.w
                for xx in range(a, e + 1):
                    self.blend(row + xx, paint(xx + 0.5, sy))

    def stroke(self, pts, width, color):
        r = max(0.6, width / 2.0)
        r2 = r * r
        for i in range(len(pts) - 1):
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            dx = x1 - x0; dy = y1 - y0; L2 = dx * dx + dy * dy
            xa = max(0, int(min(x0, x1) - r)); xb = min(self.w - 1, int(max(x0, x1) + r) + 1)
            ya = max(0, int(min(y0, y1) - r)); yb = min(self.h - 1, int(max(y0, y1) + r) + 1)
            for yy in range(ya, yb + 1):
                py = yy + 0.5
                row = yy * self.w
                for xx in range(xa, xb + 1):
                    px = xx + 0.5
                    if L2 > 0:
                        t = ((px - x0) * dx + (py - y0) * dy) / L2
                        t = 0 if t < 0 else 1 if t > 1 else t
                        qx = x0 + t * dx - px; qy = y0 + t * dy - py
                    else:
                        qx = x0 - px; qy = y0 - py
                    if qx * qx + qy * qy <= r2:
                        mark = self.stroke_mark
                        if mark[row + xx] != self.stroke_id:
                            mark[row + xx] = self.stroke_id
                            self.blend(row + xx, color)

    def to_rgba(self, ss):
        w = self.w // ss; h = self.h // ss
        out = bytearray(w * h * 4)
        n = ss * ss
        for y in range(h):
            for x in range(w):
                r = g = bb = a = 0.0
                for j in range(ss):
                    row = (y * ss + j) * self.w + x * ss
                    for i in range(ss):
                        d = self.px[row + i]; r += d[0]; g += d[1]; bb += d[2]; a += d[3]
                a /= n
                o = (y * w + x) * 4
                if a > 0:
                    out[o] = min(255, int(r / n / a * 255 + 0.5)); out[o + 1] = min(255, int(g / n / a * 255 + 0.5))
                    out[o + 2] = min(255, int(bb / n / a * 255 + 0.5)); out[o + 3] = min(255, int(a * 255 + 0.5))
        return w, h, bytes(out)


# --- the scene ----------------------------------------------------------------------

class Swf:
    def __init__(self, name):
        self.b = cached(name)
        self.ch = chars(self.b)
        self.shapes = {}
        self.bitmaps = {}

    def shape(self, cid):
        if cid not in self.shapes:
            code, p, ln = self.ch[cid]
            self.shapes[cid] = parse_shape(self.b, code, p, ln)
        return self.shapes[cid]

    def bitmap(self, bid):
        if bid not in self.bitmaps:
            self.bitmaps[bid] = None
            if bid in self.ch and self.ch[bid][0] in (20, 36):
                code, p, ln = self.ch[bid]
                cid, fmt, w, h = struct.unpack_from('<HBHH', self.b, p)
                if fmt == 5:
                    data = zlib.decompress(self.b[p + 7:p + ln])
                    self.bitmaps[bid] = (w, h, data, code == 36)
        return self.bitmaps[bid]

    def display_list(self, cid, frame):
        """depth -> [char, matrix, cxform] at `frame` (0-based) of sprite `cid`."""
        code, p, ln = self.ch[cid]
        dl = {}
        f = 0
        for c, q, l in subtags(self.b, p, ln):
            if c == 1:
                if f == frame: break
                f += 1
            elif c in (26, 70):
                bs = Bits(self.b, q)
                flags = bs.u8()
                flags2 = bs.u8() if c == 70 else 0
                depth = bs.u16()
                if c == 70 and (flags2 & 8 or (flags2 & 16 and flags & 2)): bs.string()
                ent = dl.get(depth, [None, IDENT, NOCX]) if flags & 1 else [None, IDENT, NOCX]
                if flags & 2: ent[0] = bs.u16()
                if flags & 4: ent[1] = bs.matrix()
                if flags & 8: ent[2] = bs.cxform(True)
                if flags & 0x40 and False: pass
                if ent[0] is not None: dl[depth] = ent
            elif c == 28:
                depth = struct.unpack_from('<H', self.b, q)[0]; dl.pop(depth, None)
            elif c == 5:
                depth = struct.unpack_from('<H', self.b, q + 2)[0]; dl.pop(depth, None)
        return dl

    def walk(self, cid, m, cx, frame, out, skip=()):
        """Flatten to [(shape cid, matrix, cxform)] in draw order."""
        if cid not in self.ch or cid in skip: return
        code = self.ch[cid][0]
        if code in (2, 22, 32, 83):
            out.append((cid, m, cx))
        elif code == 39:
            for depth in sorted(self.display_list(cid, frame)):
                ch, mm, cc = self.display_list(cid, frame)[depth]
                self.walk(ch, mat_mul(m, mm), cx_mul(cx, cc), 0, out, skip)

    def render(self, cid, frame, size, ss=3, pad=0.06, skip=()):
        items = []
        self.walk(cid, IDENT, NOCX, frame, items, skip)
        # bounds in twips
        xs = []; ys = []
        for sc, m, cx in items:
            for fills, lines, segs in self.shape(sc):
                for s in segs:
                    for x, y in s[3][::4] + s[3][-1:]:
                        X, Y = apply(m, x, y); xs.append(X); ys.append(Y)
        if not xs: return None
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        W = size * ss
        span = max(x1 - x0, y1 - y0) or 1
        k = W * (1 - 2 * pad) / span
        ox = (W - (x1 - x0) * k) / 2 - x0 * k
        oy = (W - (y1 - y0) * k) / 2 - y0 * k
        view = (k, 0.0, 0.0, k, ox, oy)
        cv = Canvas(W, W)
        cv.stroke_mark = [0] * (W * W); cv.stroke_id = 0
        for sc, m, cx in items:
            mm = mat_mul(view, m)
            scale = math.sqrt(abs(mm[0] * mm[3] - mm[1] * mm[2]))
            for fills, lines, segs in self.shape(sc):
                # fills
                for si in range(1, len(fills) + 1):
                    polys = []
                    for s in segs:
                        if s[0] == si or s[1] == si:
                            pts = [apply(mm, x, y) for x, y in s[3]]
                            if len(pts) > 1: polys.append(pts)
                    if not polys: continue
                    # Join open runs into closed rings by closing each on itself:
                    # even-odd over a set of edges is independent of their order.
                    closed = []
                    for pts in polys: closed.append(pts)
                    paint = self.painter(fills[si - 1], mm, cx)
                    if paint: cv.fill_polys(closed, paint)
                for li in range(1, len(lines) + 1):
                    w, col = lines[li - 1]
                    c = cx_apply(cx, col)
                    cv.stroke_id += 1
                    for s in segs:
                        if s[2] == li and len(s[3]) > 1:
                            cv.stroke([apply(mm, x, y) for x, y in s[3]], max(1.0, w * scale), c)
        return cv.to_rgba(ss)

    def painter(self, fill, mm, cx):
        kind = fill[0]
        if kind == 'solid':
            c = cx_apply(cx, fill[1]); return lambda x, y: c
        if kind in ('linear', 'radial'):
            inv = invert(mat_mul(mm, fill[1]))
            stops = [(r, cx_apply(cx, c)) for r, c in fill[2]]
            if not inv: return lambda x, y: stops[-1][1]
            def grad(t):
                t = max(0, min(255, t))
                if t <= stops[0][0]: return stops[0][1]
                for i in range(1, len(stops)):
                    if t <= stops[i][0]:
                        a, ca = stops[i - 1]; b_, cb = stops[i]
                        u = (t - a) / ((b_ - a) or 1)
                        return tuple(int(ca[j] + (cb[j] - ca[j]) * u) for j in range(4))
                return stops[-1][1]
            if kind == 'linear':
                return lambda x, y: grad((apply(inv, x, y)[0] + 16384) / 32768 * 255)
            return lambda x, y: grad(math.hypot(*apply(inv, x, y)) / 16384 * 255)
        if kind == 'bitmap':
            bm = self.bitmap(fill[1])
            if not bm: return lambda x, y: (128, 128, 128, 255)
            w, h, data, pre = bm
            inv = invert(mat_mul(mm, fill[2]))
            if not inv: return None
            def sample(x, y):
                u, v = apply(inv, x, y)
                ui = int(u); vi = int(v)
                if not (0 <= ui < w and 0 <= vi < h):
                    ui = min(w - 1, max(0, ui)); vi = min(h - 1, max(0, vi))
                o = (vi * w + ui) * 4
                a, r, g, bb = data[o], data[o + 1], data[o + 2], data[o + 3]
                if pre and a: r = min(255, r * 255 // a); g = min(255, g * 255 // a); bb = min(255, bb * 255 // a)
                return cx_apply(cx, (r, g, bb, a))
            return sample
        return None


def frame_count(swf, cid):
    code, p, ln = swf.ch[cid]
    return struct.unpack_from('<H', swf.b, p + 2)[0]


if __name__ == '__main__':
    from swf import symbols
    s = Swf(sys.argv[1]); sym = {v: k for k, v in symbols(s.b).items()}
    cid = sym[sys.argv[2]]; frame = int(sys.argv[3]); out = sys.argv[4]
    r = s.render(cid, frame, int(sys.argv[5]) if len(sys.argv) > 5 else 96)
    if r: png(out, *r); print('ok', out)
    else: print('empty')
