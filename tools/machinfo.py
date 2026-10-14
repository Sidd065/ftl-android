import struct, sys
path = sys.argv[1]
data = open(path,'rb').read()
magic = struct.unpack('>I', data[:4])[0]
slices = []
if magic == 0xcafebabe:
    n = struct.unpack('>I', data[4:8])[0]
    for i in range(n):
        cputype, cpusub, off, size, align = struct.unpack('>iiIII', data[8+i*20:28+i*20])
        slices.append((cputype, cpusub, off, size))
else:
    slices.append((None,None,0,len(data)))
LC = {0x1:'SEGMENT',0x19:'SEGMENT_64',0x2:'SYMTAB',0xb:'DYSYMTAB',0xc:'LOAD_DYLIB',0x80000022:'DYLD_INFO_ONLY',0x22:'DYLD_INFO',0x21:'ENCRYPTION_INFO',0x2c:'ENCRYPTION_INFO_64',0x80000028:'MAIN',0xe:'LOAD_DYLINKER',0x1b:'UUID',0x25:'VERSION_MIN_IPHONEOS',0x2a:'SOURCE_VERSION',0x26:'FUNCTION_STARTS',0x29:'DATA_IN_CODE',0x1d:'CODE_SIGNATURE',0x80000018:'LOAD_WEAK_DYLIB',0x8000001c:'RPATH',0x80000033:'DYLD_EXPORTS_TRIE',0x80000034:'DYLD_CHAINED_FIXUPS',0x32:'BUILD_VERSION'}
for cputype, cpusub, off, size in slices:
    d = data[off:off+size]
    m = struct.unpack('<I', d[:4])[0]
    is64 = m == 0xfeedfacf
    hdr = struct.unpack('<IiiIIII', d[:28])
    ncmds, sizeofcmds, flags = hdr[4], hdr[5], hdr[6]
    print(f"== slice cputype={hdr[1]} sub={hdr[2]} filetype={hdr[3]} ncmds={ncmds} off={off} size={size} 64={is64}")
    p = 32 if is64 else 28
    for i in range(ncmds):
        cmd, cmdsize = struct.unpack('<II', d[p:p+8])
        name = LC.get(cmd, hex(cmd))
        extra = ''
        if cmd in (0xc, 0x80000018, 0xe):
            stroff = struct.unpack('<I', d[p+8:p+12])[0]
            s = d[p+stroff:p+cmdsize].split(b'\0')[0].decode()
            extra = s
        elif cmd in (0x21, 0x2c):
            cryptoff, cryptsize, cryptid = struct.unpack('<III', d[p+8:p+20])
            extra = f"cryptoff={cryptoff:#x} cryptsize={cryptsize:#x} cryptid={cryptid}"
        elif cmd == 0x19:
            segname = d[p+8:p+24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, sflags = struct.unpack('<QQQQiiII', d[p+24:p+72])
            extra = f"{segname} vm={vmaddr:#x}+{vmsize:#x} file={fileoff:#x}+{filesize:#x} prot={initprot} nsects={nsects}"
            q = p+72
            for j in range(nsects):
                sectname = d[q:q+16].split(b'\0')[0].decode(); sseg = d[q+16:q+32].split(b'\0')[0].decode()
                addr, ssize, soff, salign, reloff, nreloc, sfl = struct.unpack('<QQIIIII', d[q+32:q+68])
                extra += f"\n      {sseg},{sectname} addr={addr:#x} size={ssize:#x} off={soff:#x} flags={sfl:#x}"
                q += 80
        elif cmd == 0x25:
            v, sdk = struct.unpack('<II', d[p+8:p+16])
            extra = f"min={v>>16}.{(v>>8)&0xff} sdk={sdk>>16}.{(sdk>>8)&0xff}"
        elif cmd == 0x80000028:
            eo, ss = struct.unpack('<QQ', d[p+8:p+24]); extra = f"entryoff={eo:#x}"
        elif cmd == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack('<IIII', d[p+8:p+24]); extra = f"nsyms={nsyms} strsize={strsize}"
        print(f"  {name} {extra}")
        p += cmdsize
