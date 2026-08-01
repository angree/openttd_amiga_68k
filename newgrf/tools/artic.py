#!/usr/bin/env python3
"""Resolve the articulated-engine callback (0x16) out of a tram GRF, statically.

Walks the Action 2 chains exactly as OpenTTD binds them (a set id resolves to
whatever definition was last seen ABOVE the referring sprite), so the answer is
what the game will actually compute.  Unknown variables read as 0.
"""
import sys
from nfoparse import load

sp = load(sys.argv[1])

# chains[setid] = list of (spritenum, parsed)
chains = {}
act3 = []           # (spritenum, engine_id, purchase_set, default_set)

def parse_var2(b):
    """Parse a variational Action 2 (type 0x80+). Returns dict or None."""
    setid, typ = b[2], b[3]
    if typ < 0x80: return None
    size = {0x81:1, 0x82:1, 0x85:2, 0x86:2, 0x89:4, 0x8A:4}.get(typ)
    if size is None: return None
    i = 4
    adjusts = []
    while True:
        var = b[i]; shift = b[i+1]; i += 2
        if var == 0x60 <= var <= 0x7F:
            i += 1
        mask = int.from_bytes(bytes(b[i:i+size]), 'little'); i += size
        adjusts.append((var, shift & 0x1F, mask))
        if not (shift & 0x20): break
        # operator byte + operand(s) - not needed for these chains, bail out
        return None
    nvar = b[i]; i += 1
    ranges = []
    for _ in range(nvar):
        res = b[i] | (b[i+1] << 8); i += 2
        lo = int.from_bytes(bytes(b[i:i+size]), 'little'); i += size
        hi = int.from_bytes(bytes(b[i:i+size]), 'little'); i += size
        ranges.append((res, lo, hi))
    default = b[i] | (b[i+1] << 8)
    return dict(setid=setid, adjusts=adjusts, ranges=ranges, default=default, nvar=nvar)

for n, b in sp:
    if not b: continue
    if b[0] == 0x02 and b[1] == 0x01:
        chains.setdefault(b[2], []).append((n, parse_var2(b)))
    elif b[0] == 0x03 and b[1] == 0x01:
        nid = b[2]
        i = 3
        ids = []
        for _ in range(nid & 0x7F):
            if b[i] == 0xFF:
                ids.append(b[i+1] | (b[i+2] << 8)); i += 3
            else:
                ids.append(b[i]); i += 1
        ncid = b[i]; i += 1
        cargo_map = {}
        for _ in range(ncid):
            ct = b[i]; cid = b[i+1] | (b[i+2] << 8); i += 3
            cargo_map[ct] = cid
        default = b[i] | (b[i+1] << 8)
        for e in ids:
            act3.append((n, e, cargo_map, default))

def bound(setid, before):
    """The definition of setid that is in force at sprite `before`."""
    best = None
    for n, p in chains.get(setid, []):
        if n < before and p is not None: best = (n, p)
    return best

VARS = {0x0C: 'callback', 0x10: 'extra', 0xF2: 'subtype'}

def resolve(setid, before, env, depth=0):
    if depth > 12: return ('LOOP', None)
    got = bound(setid, before)
    if got is None: return ('unbound-%02X' % setid, None)
    n, p = got
    if len(p['adjusts']) != 1: return ('multi-adjust', None)
    var, shift, mask = p['adjusts'][0]
    val = (env.get(var, 0) >> shift) & mask
    res = p['default']
    for r, lo, hi in p['ranges']:
        if lo <= val <= hi: res = r; break
    if res & 0x8000:
        return ('value', res & 0x7FFF)
    return resolve(res, n, env, depth + 1)

print('engine  idx  subtype -> artic callback result')
for n, eid, cmap, default in sorted(act3, key=lambda t: t[1]):
    if not (0x58 <= eid <= 0x6D): continue
    for label, start in (('ingame', default), ('buylist', cmap.get(0xFF, default))):
        out = []
        for idx in (1, 2):
            for st in (0, 1, 2):
                env = {0x0C: 0x16, 0x10: idx, 0xF2: st}
                kind, v = resolve(start, n, env)
                out.append('i%d/s%d=%s' % (idx, st, ('0x%02X' % v) if kind == 'value' else kind))
        print('0x%02X %-8s %s' % (eid, label, '  '.join(out)))
