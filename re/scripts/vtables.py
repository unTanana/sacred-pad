# Map MSVC RTTI class names to vtable addresses: TypeDescriptor <- CompleteObjectLocator <- vtable[-1].
import re, struct, sys
d = open(sys.argv[1], 'rb').read()
BASE, RDATA, DATA = 0x400000, (0x490000, 0x4e7000), (0x4e7000, 0x624000)  # file offsets == RVAs here
va = lambda off: off + BASE
names = {}
for m in re.finditer(rb'\.\?AV([A-Za-z0-9_@?$]+)@@\x00', d):
    names[va(m.start() - 8)] = m.group(1).decode()  # TypeDescriptor = {vftable, spare, name}
cols = {}
for off in range(RDATA[0], RDATA[1] - 20, 4):  # COL: sig 0, offset, cdOffset, pTypeDescriptor, pClassHierarchy
    sig, o, cd, td = struct.unpack_from('<IIII', d, off)
    if sig == 0 and td in names:
        cols[va(off)] = (names[td], o)
for off in range(RDATA[0] + 4, RDATA[1], 4):
    col = struct.unpack_from('<I', d, off - 4)[0]
    if col in cols and cols[col][1] == 0:
        print(f'{va(off):08x} {cols[col][0]}')
