#!/usr/bin/env python3
"""Parse a GRFCODEC .nfo into (spritenum, bytes) for pseudo-sprites."""
import re, sys

def _hex(s, buf):
    i = 0
    while i < len(s):
        c = s[i]
        if c.isspace(): i += 1; continue
        if c == '"':
            j = s.index('"', i+1)
            buf += s[i+1:j].encode('latin-1')
            i = j+1
            continue
        if re.match(r'[0-9A-Fa-f]{2}', s[i:i+2]):
            buf.append(int(s[i:i+2], 16)); i += 2
        else:
            i += 1

def load(path):
    out, cur = [], None
    for line in open(path, encoding='latin-1'):
        if line.startswith('//') or not line.strip():
            continue
        m = re.match(r'^\s*(\d+)\s+\*\s+(\d+)\s+(.*)$', line)
        if m:
            if cur: out.append(cur)
            cur = [int(m.group(1)), bytearray()]
            _hex(m.group(3), cur[1])
            continue
        if re.match(r'^\s*\d+\s+\S+\.(png|pcx)\b', line, re.I):
            if cur: out.append(cur)
            cur = None
            continue
        if cur is not None:
            _hex(line, cur[1])
    if cur: out.append(cur)
    return out

if __name__ == '__main__':
    sp = load(sys.argv[1])
    print(len(sp), 'pseudo-sprites', file=sys.stderr)
    for n, b in sp:
        if not b: continue
        print('%5d  act=%02X  len=%d' % (n, b[0], len(b)))
