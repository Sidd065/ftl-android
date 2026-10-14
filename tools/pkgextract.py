import struct, sys, zlib, os
f = open(sys.argv[1], 'rb')
sig, hsize, esize, count, psize = struct.unpack('>4sHHII', f.read(16))
entries = [struct.unpack('>IIIII', f.read(esize)) for _ in range(count)]
paths = f.read(psize)
outdir = sys.argv[2]; pat = sys.argv[3] if len(sys.argv) > 3 else ''
n = 0
for h, fl_off, off, size, usize in entries:
    flags = fl_off >> 24; po = fl_off & 0xffffff
    name = paths[po:paths.index(b'\0', po)].decode()
    if pat not in name: continue
    f.seek(off); data = f.read(size)
    if flags & 1: data = zlib.decompress(data)
    p = os.path.join(outdir, name); os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'wb').write(data); n += 1
print(n, "extracted")
