import struct, sys, collections
f = open(sys.argv[1], 'rb')
hdr = f.read(16)
sig, hsize, esize, count, psize = struct.unpack('>4sHHII', hdr)
entries = [struct.unpack('>IIIII', f.read(esize)) for _ in range(count)]
paths = f.read(psize)
exts = collections.Counter(); sizes = collections.Counter()
out = []
for h, fl_off, off, size, usize in entries:
    flags = fl_off >> 24; po = fl_off & 0xffffff
    name = paths[po:paths.index(b'\0', po)].decode()
    ext = name.rsplit('.',1)[-1] if '.' in name else ''
    exts[ext] += 1; sizes[ext] += usize
    out.append((name, flags, size, usize))
print(count, "entries")
for e, c in exts.most_common(): print(f"  .{e}: {c} files, {sizes[e]/1e6:.1f} MB")
tops = collections.Counter(n.split('/')[0] for n,_,_,_ in out)
print(tops.most_common(30))
if len(sys.argv) > 2:
    for n, fl, s, u in out:
        if sys.argv[2] in n: print(n, fl, s, u)
