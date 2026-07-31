"""
Neutralise .MIPS.events.* in an IRIX crt object.

These carry R_MIPS_SCN_DISP relocations that LLD 18 reports as an internal
error, and mogrix sidesteps them by using the mips3/fixed/ crt files, which
this IRIX install does not have. The sections are SGI performance-tool
metadata and nothing in a running program reads them.

Only sh_type changes, to SHT_NULL. No offsets move, so every other section
header stays correct.
"""
import struct, sys

DROP = ('.MIPS.events', '.rel.MIPS.events')

def scrub(src, dst):
    b = bytearray(open(src, 'rb').read())
    u16 = lambda o: struct.unpack_from('>H', b, o)[0]
    u32 = lambda o: struct.unpack_from('>I', b, o)[0]
    shoff, shent, shnum, shstr = u32(32), u16(46), u16(48), u16(50)
    strb = u32(shoff + shstr * shent + 16)

    n = 0
    for i in range(shnum):
        o = shoff + i * shent
        s = strb + u32(o)
        name = b[s:b.index(b'\0', s)].decode()
        if name.startswith(DROP):
            struct.pack_into('>I', b, o + 4, 0)   # sh_type = SHT_NULL
            print(f'  {name}')
            n += 1
    open(dst, 'wb').write(b)
    return n

for p in sys.argv[1:]:
    out = p + '.scrubbed'
    print(p)
    print(f'  {scrub(p, out)} sections neutralised -> {out}')
