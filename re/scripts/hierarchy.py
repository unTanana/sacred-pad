# Base classes of each RTTI class (MSVC: COL -> ClassHierarchyDescriptor -> BaseClassArray -> BaseClassDescriptor -> TD).
import re, struct, sys
d = open(sys.argv[1], 'rb').read()
B = 0x400000
u32 = lambda va: struct.unpack_from('<I', d, va - B)[0]
names = {B + m.start() - 8: m.group(1).decode() for m in re.finditer(rb'\.\?AV([A-Za-z0-9_@?$]+)@@\x00', d)}
for line in open(sys.argv[2]):
    vt, name = line.split()
    vt = int(vt, 16)
    col = u32(vt - 4)
    chd = u32(col + 16)
    n, arr = u32(chd + 8), u32(chd + 12)
    bases = [names.get(u32(u32(arr + 4 * i)), '?') for i in range(1, n)]
    if name.startswith('cUI_'):
        print(f'{vt:08x} {name:22s} : {" < ".join(bases)}')
