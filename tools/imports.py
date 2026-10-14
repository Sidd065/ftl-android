import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO
m = MachO(sys.argv[1])
print("dylibs:", list(enumerate(m.dylibs, 1)))
by = {}
for kind in ('bind','lazy','weak'):
    for addr, sym, ordn, add, typ, fl in m.binds(kind):
        lib = m.dylibs[ordn-1].split('/')[-1] if ordn > 0 else ('self' if ordn==0 else 'flat/weak')
        by.setdefault(lib, set()).add((sym, kind))
for lib in sorted(by):
    syms = sorted(by[lib])
    print(f"\n### {lib} ({len(syms)})")
    for s,k in syms: print(f"  {s}  [{k}]")
print("\nrebases:", len(m.rebases()))
print("exports:", m.exports()[:20])
