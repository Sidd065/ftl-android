import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO
m = MachO(sys.argv[1])
binds = {}
for kind in ('bind','lazy','weak'):
    for addr, sym, ordn, add, typ, fl in m.binds(kind):
        binds[addr] = sym
def sect(name):
    for s in m.sects:
        if s[1] == name: return s
def ptr(a):
    if a in binds: return ('BIND', binds[a])
    return m.u64(a)
def name_of_class(a):
    if a in binds: return binds[a]
    v = m.u64(a)
    return v
def dump_methods(ml, prefix):
    if not ml: return
    ent, cnt = struct.unpack('<II', m.read(ml, 8))
    for i in range(cnt):
        n, t, imp = struct.unpack('<QQQ', m.read(ml+8+i*24, 24))
        print(f"    {prefix}[{m.cstr(n)}] {m.cstr(t)} imp={imp:#x}")
def dump_class(ca):
    isa = m.u64(ca); sup = ca+8
    supname = binds.get(sup)
    if not supname:
        sv = m.u64(sup)
        supname = f"{sv:#x}"
        if sv:
            ro = m.u64(sv+32) & ~7
            supname = m.cstr(m.u64(ro+24))
    ro = m.u64(ca+32) & ~7
    flags, istart, isize, res, ivl, nm, meths, protos, ivars, wivl, props = struct.unpack('<IIIIQQQQQQQ', m.read(ro, 72))
    print(f"class {m.cstr(nm)} : {supname}  flags={flags:#x} instStart={istart:#x} instSize={isize:#x}  @{ca:#x}")
    if protos:
        cnt = m.u64(protos)
        for i in range(cnt):
            pa = m.u64(protos+8+i*8)
            print(f"    <proto {m.cstr(m.u64(pa+8))}>")
    if ivars:
        ent, cnt = struct.unpack('<II', m.read(ivars, 8))
        for i in range(cnt):
            off, n, t, al, sz = struct.unpack('<QQQII', m.read(ivars+8+i*ent, 32))
            print(f"    ivar {m.cstr(n)} {m.cstr(t)} offset@{off:#x}={struct.unpack('<I', m.read(off,4))[0]:#x} size={sz}")
    dump_methods(meths, '-')
    # metaclass
    if isa:
        mro = m.u64(isa+32) & ~7
        mflags, mis, miz, mres, mivl, mnm, mmeths = struct.unpack('<IIIIQQQ', m.read(mro, 40))
        dump_methods(mmeths, '+')
    if props:
        ent, cnt = struct.unpack('<II', m.read(props, 8))
        for i in range(cnt):
            n, a = struct.unpack('<QQ', m.read(props+8+i*ent, 16))
            print(f"    @property {m.cstr(n)} {m.cstr(a)}")
s = sect('__objc_classlist')
for i in range(s[3]//8):
    dump_class(m.u64(s[2]+i*8))
s = sect('__objc_nlclslist')
print("nlclslist:", [hex(m.u64(s[2]+i*8)) for i in range(s[3]//8)])
s = sect('__objc_catlist')
print("catlist:", s)
s = sect('__objc_protolist')
for i in range(s[3]//8):
    pa = m.u64(s[2]+i*8)
    print("protocol", m.cstr(m.u64(pa+8)))
print("== selrefs")
s = sect('__objc_selrefs')
sels = [m.cstr(m.u64(s[2]+i*8)) for i in range(s[3]//8)]
print(' '.join(sorted(sels)))
print("== classrefs")
s = sect('__objc_classrefs')
print(' '.join(str(binds.get(s[2]+i*8, hex(m.u64(s[2]+i*8)))) for i in range(s[3]//8)))
print("== cfstrings")
s = sect('__cfstring')
for i in range(s[3]//32):
    a = s[2]+i*32
    isa, fl, sp, ln = struct.unpack('<QQQQ', m.read(a, 32))
    print(repr(m.cstr(sp)), end=' ')
print()
