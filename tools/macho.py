# Minimal Mach-O arm64 parser for analysis (bind/lazy/weak/rebase/exports/symtab)
import struct, sys
def uleb(d, i):
    r = 0; s = 0
    while True:
        b = d[i]; i += 1
        r |= (b & 0x7f) << s; s += 7
        if not (b & 0x80): return r, i
def sleb(d, i):
    r = 0; s = 0
    while True:
        b = d[i]; i += 1
        r |= (b & 0x7f) << s; s += 7
        if not (b & 0x80):
            if b & 0x40: r -= (1 << s)
            return r, i
class MachO:
    def __init__(self, path, arch=0x100000c):
        data = open(path,'rb').read()
        magic = struct.unpack('>I', data[:4])[0]
        if magic == 0xcafebabe:
            n = struct.unpack('>I', data[4:8])[0]
            for i in range(n):
                ct, cs, off, size, al = struct.unpack('>iiIII', data[8+i*20:28+i*20])
                if ct == arch: data = data[off:off+size]; break
        self.d = data
        d = data
        ncmds = struct.unpack('<I', d[16:20])[0]
        p = 32
        self.segs = []; self.sects = []; self.dylibs = []
        while ncmds:
            cmd, cmdsize = struct.unpack('<II', d[p:p+8])
            if cmd == 0x19:
                segname = d[p+8:p+24].split(b'\0')[0].decode()
                vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack('<QQQQiiII', d[p+24:p+72])
                self.segs.append((segname, vmaddr, vmsize, fileoff, filesize))
                q = p+72
                for j in range(nsects):
                    sectname = d[q:q+16].split(b'\0')[0].decode(); sseg = d[q+16:q+32].split(b'\0')[0].decode()
                    addr, ssize, soff, salign, reloff, nreloc, sfl, r1, r2, r3 = struct.unpack('<QQIIIIIIII', d[q+32:q+80])
                    self.sects.append((sseg, sectname, addr, ssize, soff, sfl, r1))
                    q += 80
            elif cmd in (0xc, 0x80000018):
                stroff = struct.unpack('<I', d[p+8:p+12])[0]
                self.dylibs.append(d[p+stroff:p+cmdsize].split(b'\0')[0].decode())
            elif cmd in (0x22, 0x80000022):
                (self.rebase_off, self.rebase_size, self.bind_off, self.bind_size, self.weak_off, self.weak_size,
                 self.lazy_off, self.lazy_size, self.export_off, self.export_size) = struct.unpack('<10I', d[p+8:p+48])
            elif cmd == 0x2:
                self.symoff, self.nsyms, self.stroff, self.strsize = struct.unpack('<IIII', d[p+8:p+24])
            elif cmd == 0xb:
                vals = struct.unpack('<18I', d[p+8:p+80])
                self.indirectsymoff, self.nindirectsyms = vals[12], vals[13]
            p += cmdsize; ncmds -= 1
    def seg_addr(self, idx): return self.segs[idx][1]
    def read(self, addr, n):
        for s in self.segs:
            if s[1] <= addr < s[1]+s[2]:
                off = addr - s[1]
                if off + n <= s[4]: return self.d[s[3]+off:s[3]+off+n]
                return (self.d[s[3]+off:s[3]+s[4]] + b'\0'*n)[:n]
        return None
    def u64(self, addr): return struct.unpack('<Q', self.read(addr, 8))[0]
    def cstr(self, addr):
        out = b''
        while True:
            c = self.read(addr, 64)
            if c is None: return None
            z = c.find(b'\0')
            if z >= 0: return (out + c[:z]).decode('utf-8', 'replace')
            out += c; addr += 64
    def binds(self, kind='bind'):
        off, size = {'bind':(self.bind_off,self.bind_size),'weak':(self.weak_off,self.weak_size),'lazy':(self.lazy_off,self.lazy_size)}[kind]
        d = self.d; i = off; end = off+size
        res = []; ordinal = 0; sym = None; typ = 1; addend = 0; seg = 0; segoff = 0; flags = 0
        while i < end:
            b = d[i]; i += 1
            op = b & 0xf0; imm = b & 0x0f
            if op == 0x00:
                if kind == 'lazy': continue
                else: break
            elif op == 0x10: ordinal = imm
            elif op == 0x20: ordinal, i = uleb(d, i)
            elif op == 0x30: ordinal = (imm | 0xf0) - 256 if imm else 0
            elif op == 0x40:
                flags = imm; z = d.index(b'\0', i); sym = d[i:z].decode(); i = z+1
            elif op == 0x50: typ = imm
            elif op == 0x60: addend, i = sleb(d, i)
            elif op == 0x70: seg = imm; segoff, i = uleb(d, i)
            elif op == 0x80: v, i = uleb(d, i); segoff = (segoff + v) & 0xffffffffffffffff
            elif op == 0x90:
                res.append((self.seg_addr(seg)+segoff, sym, ordinal, addend, typ, flags)); segoff += 8
            elif op == 0xa0:
                res.append((self.seg_addr(seg)+segoff, sym, ordinal, addend, typ, flags)); v, i = uleb(d, i); segoff = (segoff + 8 + v) & 0xffffffffffffffff
            elif op == 0xb0:
                res.append((self.seg_addr(seg)+segoff, sym, ordinal, addend, typ, flags)); segoff += 8 + imm*8
            elif op == 0xc0:
                cnt, i = uleb(d, i); skip, i = uleb(d, i)
                for k in range(cnt):
                    res.append((self.seg_addr(seg)+segoff, sym, ordinal, addend, typ, flags)); segoff += 8 + skip
            else: raise Exception('bad bind op %x' % b)
        return res
    def rebases(self):
        d = self.d; i = self.rebase_off; end = i + self.rebase_size
        res = []; seg = 0; segoff = 0; typ = 1
        while i < end:
            b = d[i]; i += 1; op = b & 0xf0; imm = b & 0xf
            if op == 0x00: break
            elif op == 0x10: typ = imm
            elif op == 0x20: seg = imm; segoff, i = uleb(d, i)
            elif op == 0x30: v, i = uleb(d, i); segoff += v
            elif op == 0x40: segoff += imm*8
            elif op == 0x50:
                for k in range(imm): res.append(self.seg_addr(seg)+segoff); segoff += 8
            elif op == 0x60:
                c, i = uleb(d, i)
                for k in range(c): res.append(self.seg_addr(seg)+segoff); segoff += 8
            elif op == 0x70:
                res.append(self.seg_addr(seg)+segoff); v, i = uleb(d, i); segoff += 8 + v
            elif op == 0x80:
                c, i = uleb(d, i); s, i = uleb(d, i)
                for k in range(c): res.append(self.seg_addr(seg)+segoff); segoff += 8 + s
        return res
    def symbols(self):
        d = self.d; res = []
        for k in range(self.nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack('<IBBHQ', d[self.symoff+k*16:self.symoff+k*16+16])
            z = d.index(b'\0', self.stroff+n_strx)
            res.append((d[self.stroff+n_strx:z].decode('utf-8','replace'), n_type, n_sect, n_desc, n_value))
        return res
    def exports(self):
        d = self.d; base = self.export_off; res = []
        def walk(off, prefix):
            i = base + off
            tsize, i = uleb(d, i)
            if tsize:
                j = i
                flags, j = uleb(d, j)
                addr, j = uleb(d, j)
                res.append((prefix, flags, addr))
            i += tsize
            nchild = d[i]; i += 1
            for c in range(nchild):
                z = d.index(b'\0', i); s = d[i:z].decode(); i = z+1
                coff, i = uleb(d, i)
                walk(coff, prefix + s)
        if self.export_size: walk(0, '')
        return res
