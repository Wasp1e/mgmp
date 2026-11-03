# Part of tools/icons: renders the collar and item art out of the game's SWFs to
# PNG, for the collar menu (icons/ beside mgmp.dll). Run:
#   python tools/icons/icons.py <out dir>     (MEWGENICS_GPAK = path to resources.gpak)
import sys as _s, os as _o; _s.path.insert(0, _o.path.join(_o.path.dirname(_o.path.abspath(__file__)), '..'))
import os
from gpak import GPak
import zlib, struct, collections, sys
g=GPak(os.environ.get('MEWGENICS_GPAK', r'D:/SteamLibrary/steamapps/common/Mewgenics/resources.gpak'))
def load(name):
    b=g.read(name)
    sig=b[:3]
    if sig==b'CWS': b=b[:8]+zlib.decompress(b[8:])
    elif sig!=b'FWS': raise Exception('sig %r'%sig)
    return b
def tags(b):
    nbits=b[8]>>3; rect=(5+nbits*4+7)//8
    p=8+rect+4
    while p<len(b):
        h=struct.unpack_from('<H',b,p); code=h[0]>>6; ln=h[0]&63; p+=2
        if ln==63: ln=struct.unpack_from('<I',b,p)[0]; p+=4
        yield code,p,ln
        p+=ln
        if code==0: break
def symbols(b):
    out={}
    for code,p,ln in tags(b):
        if code in (76,56):
            n=struct.unpack_from('<H',b,p)[0]; q=p+2
            for _ in range(n):
                cid=struct.unpack_from('<H',b,q)[0]; q+=2
                e=b.index(b'\0',q); out[cid]=b[q:e].decode('latin1'); q=e+1
    return out
if __name__=='__main__':
    b=load(sys.argv[1])
    c=collections.Counter(code for code,_,_ in tags(b)); print(sorted(c.items()))
    s=symbols(b); print(len(s))
    for k,v in s.items():
        if any(w in v.lower() for w in sys.argv[2:]): print(k,v)
