#!/usr/bin/env python3
"""Rewrite the tram set's .nfo: non-Polish vehicle names, matching intro years,
and a single cargo for the freight trams.

Reads  sprites/poltrams.nfo  and writes it back in place (make a copy first).
Only pseudo-sprites are touched; the real sprite lines are copied verbatim.
"""
import re, sys, datetime

NFO = sys.argv[1]

# ---------------------------------------------------------------- new names --
# Every name is a real vehicle that is not Polish; the artwork is generic
# enough that the models stand in for their western/Czech counterparts.
NAMES = {
    0x58: 'Horse tram',
    0x59: 'BSI Type 1',
    0x5A: 'Graz Type 1',
    0x5B: 'Ringhoffer R',
    0x5C: 'Vienna Type M',
    0x5D: 'KSW war tram',
    0x5E: 'Aufbauwagen',
    0x5F: 'Tatra T1',
    0x60: 'Tatra K2',
    0x61: 'Tatra T4',
    0x62: 'MAN T4',
    0x63: 'Duewag MGT6D',
    0x64: 'Siemens Combino',
    0x65: 'Alstom Citadis',
    0x66: 'Tatra T4D-M',
    0x67: 'Stadler Vario',
    0x68: 'Bombardier NGT8',
    0x69: 'Horse goods tram',
    0x6A: 'Goods tram BSI 1',
    0x6B: 'Goods tram KSW',
    0x6C: 'Goods tram T1',
    0x6D: 'Goods tram T4',
}

# Intro years nudged where the new name means a different real vehicle.
# Anything not listed keeps the set's original date.
YEARS = {
    0x5C: 1930,   # Vienna Type M entered service 1927
    0x5D: 1943,   # the wartime standard car
    0x5E: 1949,   # Aufbauwagen, post-war rebuild programme
    0x5F: 1956,   # Tatra T1
    0x62: 1970,   # the MAN T4 as a new tram, not a second-hand import
    0x65: 2000,   # Alstom Citadis
}

# Per-engine Action 0 property overrides, {engine: {property: value}}.
# The set models the MAN T4 as a cheap worn second-hand buy - short life, fast
# reliability decay, bargain price. Priced and aged here as the new vehicle it
# was where it was built, in line with its neighbours (Tatra T4: cost 0x23 for
# 136 capacity; this one carries 150).
PROPS = {
    0x62: {0x11: 0x26,   # cost factor
           0x03: 0x1E,   # vehicle life, years
           0x02: 0x14},  # reliability decay

    # Stadler Vario: a fixed three-section car, not a length the player picks.
    # Clearing bit 5 of the callback mask (cargo suffix) drops it out of the
    # refit menu AND out of the two-car cap in GetNextArticPart(), which keys
    # on exactly that bit - so it builds whole, the way its sections, each a
    # different length, are drawn.
    0x67: {0x17: 0x19},
}

# Purchase-list blurbs (generic text ids). The set tells the player to use the
# refit menu to add trailers; this port has no refit for road vehicles and gives
# the two-car consist outright, so that instruction is simply wrong here.
BLURBS = {
    0xD006: ' 18 sitting and 22 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 3 days',
    0xD007: ' 24 sitting and 32 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 4 days',
    0xD008: ' 24 sitting and 52 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 6 days',
    0xD009: ' 16 sitting and 65 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 6 days',
    0xD00A: ' 21 sitting and 101 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 5 days',
    0xD00B: ' 32 sitting and 152 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 7 days',
    0xD00C: ' 20 sitting and 116 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 6 days',
    0xD00D: ' 30 sitting and 120 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 9 days',
    0xD011: ' 20 sitting and 76 standing places per car. A trailer can be added in the refit menu. \rLoad/unload time: 4 days',
    0xD012: ' 24 sitting and 102 standing places per car. Running costs depend on actual speed. \rLoad/unload time: 5 days',
    0xD014: ' Freight tram for goods.',
}

FREIGHT = (0x6A, 0x6B, 0x6C, 0x6D)
CARGO_GOODS = 0x05        # one cargo, the same slot the set already used

# road vehicle / common property sizes (variable ones handled explicitly)
SZ = {0x00:2,0x02:1,0x03:1,0x04:1,0x06:1,0x07:1,
      0x05:1,0x08:1,0x09:1,0x0A:4,0x0E:1,0x0F:1,
      0x10:1,0x11:1,0x12:1,0x13:1,0x14:1,0x15:1,0x16:4,0x17:1,0x18:1,0x19:1,
      0x1A:1,0x1B:1,0x1C:1,0x1D:2,0x1E:2,0x1F:4,0x21:1,0x22:2,0x23:1}

def days(year):
    return (datetime.date(year, 1, 1) - datetime.date(1, 1, 1)).days + 366

# ------------------------------------------------------------------ parsing --
def hexbytes(s, buf):
    i = 0
    while i < len(s):
        c = s[i]
        if c.isspace(): i += 1; continue
        if c == '"':
            j = s.index('"', i+1); buf += s[i+1:j].encode('latin-1'); i = j+1; continue
        if re.match(r'[0-9A-Fa-f]{2}', s[i:i+2]):
            buf.append(int(s[i:i+2], 16)); i += 2
        else:
            i += 1

def emit(num, b):
    # grfcodec wants data on the header line itself - a bare "N * len" line is
    # read as a zero-byte pseudo-sprite and refused.
    out = []
    line = '%5d * %d\t' % (num, len(b))
    for k, byte in enumerate(b):
        line += ' %02X' % byte
        if (k + 1) % 32 == 0:
            out.append(line); line = '\t'
    if line.strip(): out.append(line)
    return '\n'.join(out) + '\n'

# ------------------------------------------------------------------- edits ---
def edit_action4(b):
    feat, lang, num = b[1], b[2], b[3]
    if lang & 0x80:
        # generic text block: <04> <feat> <lang|80> <num> <word id> <strings...>
        first = b[4] | (b[5] << 8)
        parts = bytes(b[6:]).split(b'\x00')
        if len(parts) < num or not any((first + k) in BLURBS for k in range(num)):
            return None
        out = bytearray(b[:6])
        for k in range(num):
            txt = BLURBS.get(first + k)
            out += (txt.encode('latin-1') if txt is not None else parts[k]) + b'\x00'
        return out
    if feat != 0x01: return None
    i = 4
    if b[4] == 0xFF:
        eid = b[5] | (b[6] << 8); i = 7
    else:
        eid = b[4]; i = 5
    if num != 1 or eid not in NAMES: return None
    return bytearray(b[:i]) + NAMES[eid].encode('ascii') + b'\x00'

def edit_action8(b):
    """Set name and description. GPL v2 asks a modified work to say so, and the
    NewGRF window is where a player will look."""
    name = 'Tram Set (AmiTTD edition) v7068M'
    desc = ('Trams from 1866 to 2015, renamed to their non-Polish equivalents for '
            'the AmiTTD Amiga 68k port. Freight trams carry a single cargo, and '
            'consist length is set by the game rather than by the refit menu.\r'
            '(c)2014 Sojita, Voyager One (graphics), McZapkie (NML code, graphics)\r'
            'Modified 2026 for AmiTTD. License: GPL v2')
    return bytearray(b[:7]) + name.encode('latin-1') + b'\x00' + desc.encode('latin-1') + b'\x00'

def edit_action0(b):
    feat, nprop, ninfo = b[1], b[2], b[3]
    if feat != 0x01 or ninfo != 1: return None
    i = 4
    if b[i] == 0xFF:
        eid = b[i+1] | (b[i+2] << 8); i += 3
    else:
        eid = b[i]; i += 1
    b = bytearray(b)
    changed = False
    props_seen = {}
    j = i
    for _ in range(nprop):
        if j >= len(b): break
        p = b[j]; j += 1
        if p in (0x24, 0x25):          # CTT refit list: count + that many bytes
            cnt = b[j]; j += 1 + cnt; continue
        if p == 0x20:                  # sort order, extended byte
            j += 3 if b[j] == 0xFF else 1; continue
        sz = SZ.get(p)
        if sz is None: return None     # unknown -> leave this block alone
        props_seen[p] = (j, sz)
        j += sz
    if j != len(b): return None        # parse did not line up; do not touch

    for prop, value in PROPS.get(eid, {}).items():
        if prop in props_seen:
            off, sz = props_seen[prop]
            b[off:off+sz] = value.to_bytes(sz, 'little')
            changed = True

    if eid in YEARS and 0x1F in props_seen:
        off, sz = props_seen[0x1F]
        b[off:off+sz] = days(YEARS[eid]).to_bytes(4, 'little')
        changed = True

    # Only the main Action 0 block carries the cargo properties; the set also
    # emits a one-property block for the callback mask, which must be left be.
    if eid in FREIGHT and (0x1D in props_seen or 0x16 in props_seen):
        for p in (0x1D, 0x1E):         # drop the cargo-class refit options
            if p in props_seen:
                off, sz = props_seen[p]
                b[off:off+sz] = (0).to_bytes(sz, 'little'); changed = True
        if 0x16 in props_seen:         # explicit refit mask: none
            off, sz = props_seen[0x16]
            b[off:off+sz] = (0).to_bytes(sz, 'little'); changed = True
        if 0x10 in props_seen:         # the one cargo it carries
            off, sz = props_seen[0x10]
            b[off] = CARGO_GOODS; changed = True
        else:
            b += bytes([0x10, CARGO_GOODS])
            b[2] = nprop + 1; changed = True
    return b if changed else None

# -------------------------------------------------------------------- main ---
src = open(NFO, encoding='latin-1').read().split('\n')
out = []
pending = None      # [num, bytearray, [original lines]]

def flush():
    """Emit the buffered pseudo-sprite: the ORIGINAL lines unless we changed it.

    The .nfo is not pure hex - it also carries escape tokens (\\D=, \\2*, \\7! and
    friends, listed in its own header) for action D/7/9/2 operators. Re-emitting
    every sprite through our own byte parser silently dropped those and produced
    a GRF that died with "Read past end of pseudo-sprite". So only the sprites
    we actually edit are regenerated, and those are checked to be escape-free."""
    global pending
    if pending is None: return
    num, b, lines = pending
    pending = None
    nb = None
    if b:
        if b[0] == 0x04: nb = edit_action4(b)
        elif b[0] == 0x00: nb = edit_action0(b)
        elif b[0] == 0x08: nb = edit_action8(b)
    if nb is None:
        out.extend(l + '\n' for l in lines)
        return
    if any('\\' in l for l in lines):
        sys.exit('sprite %d contains escapes and cannot be regenerated' % num)
    out.append(emit(num, nb))
    stats.append((num, b[0]))

stats = []
for line in src:
    if line.startswith('//') or not line.strip():
        flush(); out.append(line + '\n'); continue
    m = re.match(r'^\s*(\d+)\s+\*\s+(\d+)\s+(.*)$', line)
    if m:
        flush()
        pending = [int(m.group(1)), bytearray(), [line]]
        hexbytes(m.group(3), pending[1]); continue
    if re.match(r'^\s*\d+\s+\S+\.(png|pcx)\b', line, re.I):
        flush(); out.append(line + '\n'); continue
    if pending is not None:
        pending[2].append(line)
        hexbytes(line, pending[1]); continue
    out.append(line + '\n')
flush()

open(NFO, 'w', encoding='latin-1').write(''.join(out))
print('rewrote %d pseudo-sprites (%d Action 4, %d Action 0)' % (
    len(stats), sum(1 for _, a in stats if a == 4), sum(1 for _, a in stats if a == 0)))
