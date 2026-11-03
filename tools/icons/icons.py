# Part of tools/icons: renders the collar and item art out of the game's SWFs to
# PNG, for the collar menu (icons/ beside mgmp.dll). Run:
#   python tools/icons/icons.py <out dir>     (MEWGENICS_GPAK = path to resources.gpak)
import sys as _s, os as _o; _s.path.insert(0, _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), '..'))
import os, re, struct, sys
from gpak import GPak
from rast import Swf
from swf import symbols
from bmp import png
from sprites import subtags
OUT = sys.argv[1]; os.makedirs(OUT, exist_ok=True)
g = GPak(os.environ.get('MEWGENICS_GPAK', r'D:/SteamLibrary/steamapps/common/Mewgenics/resources.gpak'))
# collars: every labelled frame of ClassCollar
ui = Swf('swfs/ui.swf'); sym = {v: k for k, v in symbols(ui.b).items()}; sym_ui = sym
cid = sym['ClassCollar']; code, p, ln = ui.ch[cid]; f = 0
for c, q, l in subtags(ui.b, p, ln):
    if c == 1: f += 1
    elif c == 43:
        name = ui.b[q:ui.b.index(b'\0', q)].decode()
        r = ui.render(cid, f, 64)
        if r: png(os.path.join(OUT, 'collar_%s.png' % name), *r)
print('collars done')
# items: (kind, frame) pairs the gon files use
pairs = set()
for k in g.entries:
    if not k.startswith('data/items/'): continue
    t = g.read(k).decode('utf-8', 'replace')
    for m in re.finditer(r'\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}', t):
        body = m.group(1)
        kind = re.search(r'\bkind\s+(\w+)', body); fr = re.search(r'\bframe\s+(\d+)', body)
        if kind and fr: pairs.add((kind.group(1), int(fr.group(1))))
print(len(pairs), 'item icons')
cp = Swf('swfs/catparts.swf'); sym = {v: k for k, v in symbols(cp.b).items()}
sprite = {'head': 'HeadItemIcon', 'face': 'FaceItemIcon', 'neck': 'NeckItemIcon',
          'weapon': 'WeaponIcon', 'trinket': 'TrinketIcon'}
done = 0
for kind, fr in sorted(pairs):
    if kind not in sprite: continue
    path = os.path.join(OUT, '%s_%d.png' % (kind, fr))
    if os.path.exists(path): continue
    try:
        r = cp.render(sym[sprite[kind]], fr - 1, 64, skip=(17, 42))
    except Exception as e:
        print('!!', kind, fr, e); continue
    if r: png(path, *r); done += 1
print('items done', done)
# the empty-slot silhouettes: catparts.swf sprite 42, one frame per kind (the
# corner badge of every item icon, and what an empty box shows)
for f, n in ((0, 'weapon'), (1, 'head'), (2, 'neck'), (3, 'face'), (4, 'trinket')):
    r = cp.render(42, f, 64)
    if r: png(os.path.join(OUT, 'slot_%s.png' % n), *r)
print('slots done')
# the heart that marks the second player's cats in the menu: a font icon, drawn
# white, tinted when shown
r = ui.render(sym_ui['FontIcon_heart'], 0, 64)
if r: png(os.path.join(OUT, 'heart.png'), *r)
