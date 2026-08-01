#!/usr/bin/env python3
"""List Action 0 (feature 01) blocks: which engine ids get which properties."""
import sys
from nfoparse import load

# road vehicle property sizes, OpenTTD 1.0.5 + our backport
SZ = {0x00:2,0x02:1,0x03:1,0x04:1,0x06:1,0x07:1,
      0x05:1,0x08:1,0x09:1,0x0A:4,0x0E:1,0x0F:1,
      0x10:1,0x11:1,0x12:1,0x13:1,0x14:1,0x15:1,0x16:4,0x17:1,0x18:1,0x19:1,
      0x1A:1,0x1B:1,0x1C:1,0x1D:2,0x1E:2,0x1F:4,0x21:1,0x22:2,0x23:1}
NAMES = {0x00:'intro',0x02:'reldecay',0x03:'life',0x04:'modellife',0x06:'climates',
         0x07:'loadspd',0x05:'roadtype',0x08:'speed',0x09:'runcost',0x0A:'runcostbase',
         0x0E:'spriteid',0x0F:'capacity',0x10:'cargo',0x11:'cost',0x12:'sfx',
         0x13:'power',0x14:'weight',0x15:'speed_mph',0x16:'refitmask',0x17:'cbmask',
         0x18:'tract',0x19:'airdrag',0x1A:'refitcost',0x1B:'retire',0x1C:'miscflags',
         0x1D:'cargoclass_ok',0x1E:'cargoclass_no',0x1F:'intro_long',0x21:'visual',
         0x22:'cargoage',0x23:'shorter'}

ids = {}
for n, b in load(sys.argv[1]):
    if not b or b[0] != 0x00: continue
    feat, nprop, ninfo = b[1], b[2], b[3]
    i = 4
    if b[i] == 0xFF:
        first = b[i+1] | (b[i+2] << 8); i += 3
    else:
        first = b[i]; i += 1
    if feat != 0x01: continue
    props = []
    ok = True
    for _ in range(nprop):
        if i >= len(b): ok = False; break
        p = b[i]; i += 1
        sz = SZ.get(p)
        if sz is None:
            props.append((p, None)); ok = False; break
        vals = []
        for k in range(ninfo):
            v = int.from_bytes(bytes(b[i:i+sz]), 'little'); i += sz
            vals.append(v)
        props.append((p, vals))
    print('sprite %-5d ids 0x%02X..0x%02X  props: %s%s' % (
        n, first, first+ninfo-1,
        ' '.join('%s(%02X)=%s' % (NAMES.get(p, '?'), p, v if v is None else ('/'.join(hex(x) for x in v))) for p, v in props),
        '' if ok else '  <TRUNCATED>'))
