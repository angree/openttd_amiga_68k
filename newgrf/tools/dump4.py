#!/usr/bin/env python3
"""Dump Action 4 vehicle names (feature 01, English) from a GRFCODEC .nfo."""
import sys
from nfoparse import load

rows = []
for n, b in load(sys.argv[1]):
    if not b or b[0] != 0x04: continue
    feat, lang, num = b[1], b[2], b[3]
    generic = bool(lang & 0x80)
    i = 4
    if generic:
        eid = b[4] | (b[5] << 8); i = 6
    elif feat <= 0x03:
        if b[4] == 0xFF:
            eid = b[5] | (b[6] << 8); i = 7
        else:
            eid = b[4]; i = 5
    else:
        eid = b[4]; i = 5
    lang &= 0x7F
    if feat != 0x01 or lang != 0x7F: continue
    parts = bytes(b[i:]).split(b'\x00')
    for k in range(num):
        if k < len(parts):
            rows.append((eid + k, n, parts[k].decode('utf-8', 'replace')))

for eid, n, txt in sorted(rows):
    print('id=0x%02X  sprite=%-5d  %s' % (eid, n, txt))
