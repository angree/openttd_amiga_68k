#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make-info.py  --  Convert a PNG into a classic AmigaOS Workbench icon (.info / DiskObject)
                  whose PRIMARY colour representation is *NewIcons* (the classic tooltype
                  IM1=/IM2= encoding) -- the only colour icon format the target Workbench
                  actually renders.

Dual payload:
  (1) OldIcon  : standard OS1.x/3.1 planar image, 2 bitplanes, quantised (NEAREST colour, no
                 dither) to the four reserved Workbench pens (0=grey 1=black 2=white 3=blue).
                 Fully opaque -- a solid readable fallback for systems without NewIcons.
  (2) NewIcons : full-colour (up to 256 colours) image stored in the DiskObject ToolTypes as
                 IM1=/IM2= text lines, RLE (zero-run) + 7-bit ASCII armoured, exactly as the
                 classic newicon.library / AROS icon.library decoder expects.

The NewIcons encoder is verified byte-exact by round-tripping through a faithful Python port
of the AROS icon.library `DecodeNI` routine (workbench/libs/icon/diskobjNIio.c).

All DiskObject/Gadget/Image multi-byte values are big-endian (m68k). Struct sizes:
Gadget = 44 bytes, DiskObject on-disk = 78 bytes, Image = 20 bytes.

Usage:
    python build/make-info.py
    python build/make-info.py --in <png> --out <info> --maxw 46 --maxh 46 --colors 256

Target file the icon decorates:  release/openttd-amiga-68k/openttd  (a WBTOOL, no extension)
"""

import os
import sys
import math
import struct
import argparse

from PIL import Image

# ----------------------------------------------------------------------------- constants

DO_MAGIC   = 0xE310      # WB_DISKMAGIC
DO_VERSION = 1           # WB_DISKVERSION

WBTOOL = 3               # do_Type

# Gadget.Flags
GFLG_GADGHCOMP  = 0x0000   # highlight by complement (single image, always works)
GFLG_GADGIMAGE  = 0x0004   # GadgetRender points to an Image

# Gadget.Activation
GACT_RELVERIFY     = 0x0001
GACT_GADGIMMEDIATE = 0x0002

GTYP_BOOLGADGET = 0x0001

NO_ICON_POSITION = -0x80000000   # 0x80000000

STACK_SIZE = 1000000     # do_StackSize -- MUST be 1 MB or launching from Workbench hangs

# Standard 4-colour Workbench palette (OldIcon uses ONLY these reserved pens)
WB_PALETTE = [
    (0x95, 0x95, 0x95),   # pen 0  grey
    (0x00, 0x00, 0x00),   # pen 1  black
    (0xFF, 0xFF, 0xFF),   # pen 2  white
    (0x3B, 0x67, 0xA2),   # pen 3  blue
]

# NewIcons guard tooltypes: real files carry a single-space entry, then the message.
NI_SPACE = b" "
NI_GUARD = b"*** DON'T EDIT THE FOLLOWING LINES!! ***"


def be(fmt, *vals):
    return struct.pack(">" + fmt, *vals)


# ============================================================================= OldIcon

def nearest_to_palette(rgb_img, palette):
    """Nearest-colour quantise (NO dithering) onto a small fixed palette. Returns idx grid."""
    w, h = rgb_img.size
    src = rgb_img.convert("RGB").load()
    idx = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            best, bi = 1 << 30, 0
            for i, (pr, pg, pb) in enumerate(palette):
                d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                if d < best:
                    best, bi = d, i
            idx[y][x] = bi
    return idx


def pack_bitplanes(idx, w, h, depth):
    """2-D palette indices -> Amiga planar bitmap data. Row padded to a 16-bit word;
    leftmost pixel = MSB (bit 7) of the first byte. Returns (bytes, bytes_per_row)."""
    bpr = ((w + 15) // 16) * 2
    out = bytearray()
    for plane in range(depth):
        for y in range(h):
            row = bytearray(bpr)
            for x in range(w):
                if (idx[y][x] >> plane) & 1:
                    row[x >> 3] |= (0x80 >> (x & 7))
            out += row
    return bytes(out), bpr


# ============================================================================= NewIcons encode

def ni_bits_for(numcols):
    """Bits per image entry, IDENTICAL to AROS: for(bits=1;(1<<bits)<numcols;bits++)."""
    bits = 1
    while (1 << bits) < numcols:
        bits += 1
    return bits


def entries_to_groups(chunk, bits):
    """Pack entries (each `bits` bits, MSB first) into 7-bit groups; pad last group w/ zeros."""
    bitbuf = 0
    nb = 0
    groups = []
    m = (1 << bits) - 1
    for e in chunk:
        bitbuf = (bitbuf << bits) | (e & m)
        nb += bits
        while nb >= 7:
            groups.append((bitbuf >> (nb - 7)) & 0x7F)
            nb -= 7
    if nb > 0:
        groups.append((bitbuf << (7 - nb)) & 0x7F)   # pad low bits with zero
    return groups


def groups_to_chars(groups):
    """7-bit groups -> NewIcons ASCII armour.
       value 0x00..0x4F -> byte value+0x20 ; 0x50..0x7F -> byte value+0x51.
       Runs of zero groups -> zero-run bytes 0xD1..0xFF (0xD1 = 1 group ... 0xFF = 47 groups)."""
    out = bytearray()
    i = 0
    n = len(groups)
    while i < n:
        if groups[i] == 0:
            run = 1
            while i + run < n and groups[i + run] == 0 and run < 47:
                run += 1
            out.append(0xD1 + (run - 1))
            i += run
        else:
            v = groups[i]
            out.append(v + 0x20 if v <= 0x4F else v + 0x51)
            i += 1
    return bytes(out)


def pack_lines(entries, bits, first_budget, other_budget):
    """
    Split `entries` into per-line chunks. Every NON-final line carries a multiple of `step`
    entries so its total bit count is a multiple of 7 (no straddle across the decoder's
    per-line numbits reset). The final line takes the remainder (the decoder's entry-count
    guard stops it, so trailing pad bits are harmless).
    `first_budget`/`other_budget` = max DATA chars (excluding the IMx= prefix / header) per line.
    Returns list of entry-lists.
    """
    step = 7 // math.gcd(7, bits)

    def max_entries(budget):
        e = (budget * 7) // bits
        e -= e % step
        return max(step, e)

    lines = []
    i = 0
    n = len(entries)
    first = True
    while i < n:
        budget = first_budget if first else other_budget
        cap = max_entries(budget)
        if n - i <= cap:
            lines.append(entries[i:n]); i = n
        else:
            lines.append(entries[i:i + cap]); i += cap
        first = False
    return lines


def build_im_strings(which, palette_bytes, index_bytes, width, height, numcols, transp):
    """Build the list of IM(1+which)= tooltype byte-strings for one image."""
    bits = ni_bits_for(numcols)
    prefix = b"IM" + bytes([ord('1') + which]) + b"="       # "IM1=" / "IM2="
    header5 = bytes([
        ord('B') if transp else ord('C'),
        (width  + 0x21) & 0xFF,
        (height + 0x21) & 0xFF,
        ((numcols >> 6) + 0x21) & 0xFF,
        ((numcols & 0x3F) + 0x21) & 0xFF,
    ])

    limit = 127                       # max string length excluding the terminating NUL
    pfx = len(prefix)                 # 4

    # --- palette stream (8-bit entries). First line also carries the 5 header bytes. ---
    pal_entries = list(palette_bytes)
    pal_lines = pack_lines(pal_entries, 8,
                           first_budget=limit - pfx - len(header5),   # 127-4-5 = 118
                           other_budget=limit - pfx)                  # 127-4   = 123

    # --- image stream. Starts on a FRESH line (palette and image never share a line). ---
    img_entries = list(index_bytes)
    img_lines = pack_lines(img_entries, bits,
                           first_budget=limit - pfx,
                           other_budget=limit - pfx)

    strings = []
    for k, chunk in enumerate(pal_lines):
        data = groups_to_chars(entries_to_groups(chunk, 8))
        if k == 0:
            strings.append(prefix + header5 + data)
        else:
            strings.append(prefix + data)
    for chunk in img_lines:
        data = groups_to_chars(entries_to_groups(chunk, bits))
        strings.append(prefix + data)

    for s in strings:
        assert len(s) <= limit, (which, len(s))
    return strings


# ============================================================================= NewIcons decode (AROS port)

def decode_ni_stream(lines, bits, entries, is_palette):
    """
    Faithful port of AROS icon.library DecodeNI (workbench/libs/icon/diskobjNIio.c).
    `lines` = the ordered IMx= strings available to this decode pass.
    Returns (values, lines_consumed).  For palette, data starts at offset 9 of line 0 and
    line 0 is consumed up-front; for image, the first line is loaded on demand at offset 4.
    """
    out = []
    numbits = 0
    bitbuf = 0
    loop = 0
    mask = (1 << bits) - 1

    if is_palette:
        cur = lines[0]
        pos = 9
        li = 1                     # line 0 already loaded/consumed
    else:
        cur = b""
        pos = 0
        li = 0

    while len(out) < entries:
        if loop:
            byte = 0
            loop -= 1
        else:
            if pos >= len(cur):
                if li >= len(lines):
                    raise ValueError("NewIcon data truncated")
                cur = lines[li]
                li += 1
                if not (cur[0:1] == b"I" and cur[1:2] == b"M" and cur[3:4] == b"="):
                    raise ValueError("NewIcon data invalid prefix")
                pos = 4
                numbits = 0
            byte = cur[pos]
            pos += 1
            if byte == 0:
                raise ValueError("NewIcon data invalid (NUL)")
            elif byte < 0xA0:
                byte -= 0x20
            elif byte < 0xD1:
                byte -= 0x51
            else:
                loop = byte - 0xD1
                byte = 0

        bitbuf = (bitbuf << 7) + byte
        numbits += 7
        while numbits >= bits and len(out) < entries:
            out.append((bitbuf >> (numbits - bits)) & mask)
            numbits -= bits

    return out, li


def decode_newicon(tooltypes, which):
    """
    Mirror ReadIconNI/ReadImageNI: find the guard, then decode IM(1+which)=.
    tooltypes: full ordered list of tooltype byte-strings.
    Returns dict(width,height,numcols,transp,palette,indices).
    """
    tt = [bytes(s) for s in tooltypes]
    gi = next((i for i, s in enumerate(tt) if s == NI_GUARD), None)
    if gi is None:
        raise ValueError("guard tooltype not found")

    # collect IM(1+which)= lines in order (they follow the guard)
    tag = b"IM" + bytes([ord('1') + which]) + b"="
    lines = [s for s in tt[gi + 1:] if s[:4] == tag]
    if not lines:
        raise ValueError("no %s lines" % tag.decode())

    hdr = lines[0][4:]
    width = hdr[1] - 0x21
    height = hdr[2] - 0x21
    numcols = ((hdr[3] - 0x21) << 6) + (hdr[4] - 0x21)
    transp = (hdr[0] == ord('B'))
    bits = ni_bits_for(numcols)

    pal_vals, consumed = decode_ni_stream(lines, 8, numcols * 3, is_palette=True)
    img_vals, _ = decode_ni_stream(lines[consumed:], bits, width * height, is_palette=False)

    return dict(width=width, height=height, numcols=numcols, transp=transp,
                palette=bytes(pal_vals), indices=bytes(img_vals))


# ============================================================================= DiskObject / Image

def build_diskobject(w, h, has_tooltypes):
    """78-byte on-disk DiskObject with an embedded 44-byte Gadget."""
    g = b""
    g += be("I", 0)                                    # NextGadget
    g += be("hh", 0, 0)                                # LeftEdge, TopEdge
    g += be("hh", w, h)                                # Width, Height
    g += be("H", GFLG_GADGIMAGE | GFLG_GADGHCOMP)      # Flags
    g += be("H", GACT_RELVERIFY | GACT_GADGIMMEDIATE)  # Activation
    g += be("H", GTYP_BOOLGADGET)                      # GadgetType
    g += be("I", 1)                                    # GadgetRender  (image present)
    g += be("I", 0)                                    # SelectRender
    g += be("I", 0)                                    # GadgetText
    g += be("i", 0)                                    # MutualExclude
    g += be("I", 0)                                    # SpecialInfo
    g += be("H", 0)                                    # GadgetID
    g += be("I", 0)                                    # UserData
    assert len(g) == 44, len(g)

    do = b""
    do += be("H", DO_MAGIC)                            # do_Magic
    do += be("H", DO_VERSION)                          # do_Version
    do += g                                            # do_Gadget
    do += be("B", WBTOOL)                              # do_Type
    do += be("B", 0)                                   # pad
    do += be("I", 0)                                   # do_DefaultTool  (none)
    do += be("I", 1 if has_tooltypes else 0)           # do_ToolTypes    (nonzero marker)
    do += be("i", NO_ICON_POSITION)                    # do_CurrentX
    do += be("i", NO_ICON_POSITION)                    # do_CurrentY
    do += be("I", 0)                                   # do_DrawerData
    do += be("I", 0)                                   # do_ToolWindow
    do += be("i", STACK_SIZE)                          # do_StackSize
    assert len(do) == 78, len(do)
    return do


def build_image(w, h, depth, planedata, planepick=0x03, planeonoff=0x00):
    """20-byte struct Image header followed by the planar bitmap data."""
    img = b""
    img += be("hh", 0, 0)                              # LeftEdge, TopEdge
    img += be("hh", w, h)                              # Width, Height
    img += be("h", depth)                              # Depth
    img += be("I", 1)                                  # ImageData (nonzero marker)
    img += be("B", planepick)                          # PlanePick
    img += be("B", planeonoff)                         # PlaneOnOff
    img += be("I", 0)                                  # NextImage
    assert len(img) == 20, len(img)
    return img + planedata


def serialize_tooltypes(tooltypes):
    """Icon-file ToolTypes block: LONG (n+1)*4, then for each string LONG (len+1) + bytes+NUL."""
    out = be("I", (len(tooltypes) + 1) * 4)
    for s in tooltypes:
        out += be("I", len(s) + 1) + s + b"\x00"
    return out


# ============================================================================= verify

def verify(path, exp_w, exp_h, src_ni):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    magic, ver = struct.unpack_from(">HH", data, off); off += 4
    (_ng, _le, _te, gw, gh, flags, _act, _gt,
     grender, srender, _txt, _mx, _si, _id, _ud) = struct.unpack_from(">IhhhhHHHIIIiIHI", data, off)
    off += 44
    do_type, _pad = struct.unpack_from(">BB", data, off); off += 2
    (deftool, tooltypes_ptr, _cx, _cy, drawer, _tw, stack) = struct.unpack_from(">IIiiIIi", data, off)
    off += 28
    assert off == 78, off

    # Image 1
    (_ile, _ite, iw, ih, idepth, _idata, _pp, _po, _ni) = struct.unpack_from(">hhhhhIBBI", data, off)
    off += 20
    bpr = ((iw + 15) // 16) * 2
    planelen = bpr * ih * idepth
    off += planelen

    # ToolTypes
    tts = []
    if tooltypes_ptr:
        first = struct.unpack_from(">I", data, off)[0]; off += 4
        n = first // 4 - 1
        for _ in range(n):
            slen = struct.unpack_from(">I", data, off)[0]; off += 4
            s = data[off:off + slen - 1]; off += slen   # drop trailing NUL
            tts.append(s)

    # Decode NewIcons back through the AROS DecodeNI port and compare with what we encoded.
    ni1 = decode_newicon(tts, 0)
    ni2 = decode_newicon(tts, 1)
    rt_ok = (ni1["width"] == exp_w and ni1["height"] == exp_h and
             ni1["numcols"] == src_ni["numcols"] and
             ni1["palette"] == src_ni["palette"] and
             ni1["indices"] == src_ni["indices"] and
             ni2["indices"] == src_ni["indices"] and ni2["palette"] == src_ni["palette"])

    summary = {
        "file": path,
        "bytes": len(data),
        "do_Magic": hex(magic),
        "do_Version": ver,
        "do_Type": do_type,
        "do_StackSize": stack,
        "gadget_W_H": (gw, gh),
        "GadgetRender_present": grender != 0,
        "do_DefaultTool": deftool,
        "do_ToolTypes_present": tooltypes_ptr != 0,
        "num_tooltypes": len(tts),
        "oldicon_W_H_depth": (iw, ih, idepth),
        "oldicon_plane_bytes": planelen,
        "newicon_IM1_W_H_cols_transp": (ni1["width"], ni1["height"], ni1["numcols"], ni1["transp"]),
        "newicon_IM2_present": True,
        "newicon_roundtrip_ok": rt_ok,
    }
    ok = (magic == DO_MAGIC and ver == DO_VERSION and do_type == WBTOOL and
          stack == STACK_SIZE and gw == exp_w and gh == exp_h and grender != 0 and
          tooltypes_ptr != 0 and rt_ok)
    return ok, summary, ni1


def render_newicon(ni, scale=6, bg=(0x95, 0x95, 0x95)):
    """Reconstruct the decoded NewIcons image to an RGB PIL image (for eyeballing)."""
    w, h, nc, transp = ni["width"], ni["height"], ni["numcols"], ni["transp"]
    pal = ni["palette"]
    idx = ni["indices"]
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            i = idx[y * w + x]
            if transp and i == 0:
                px[x, y] = bg
            else:
                px[x, y] = (pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])
    return im.resize((w * scale, h * scale), Image.NEAREST)


# ============================================================================= main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src",
                    default=r"I:\GITHUB\Amiga_OpenTTD\amiga_openttd2.png")
    ap.add_argument("--out", dest="out",
                    default=r"I:\GITHUB\Amiga_OpenTTD\release\openttd-amiga-68k\openttd.info")
    ap.add_argument("--maxw", type=int, default=45)
    ap.add_argument("--maxh", type=int, default=45)
    ap.add_argument("--colors", type=int, default=256)
    ap.add_argument("--preview", default=None)
    args = ap.parse_args()

    src = Image.open(args.src).convert("RGB")
    sw, sh = src.size
    print(f"source: {args.src}  {sw}x{sh}")

    scale = min(args.maxw / sw, args.maxh / sh)
    tw = max(1, int(round(sw * scale)))
    th = max(1, int(round(sh * scale)))
    print(f"target: {tw}x{th}  (opaque, no transparency)")

    rgb_t = src.resize((tw, th), Image.LANCZOS)

    # ---- OldIcon fallback: NEAREST colour to the 4 WB pens, no dithering, fully opaque ----
    old_idx = nearest_to_palette(rgb_t, WB_PALETTE)
    planes, bpr = pack_bitplanes(old_idx, tw, th, depth=2)
    print(f"OldIcon: depth=2 bpr={bpr} plane_data={len(planes)} bytes (nearest, opaque)")

    # ---- NewIcons: quantise to up to `colors`, build palette + indices, fully opaque ----
    ncolors = max(2, min(256, args.colors))
    q = rgb_t.convert("RGB").quantize(colors=ncolors, method=Image.MEDIANCUT)
    numcols = 256                                    # store a full 256-entry palette
    q_idx = q.load()
    flat = q.getpalette() or []
    palette = bytearray(numcols * 3)
    for i in range(min(len(flat), numcols * 3)):
        palette[i] = flat[i]
    indices = bytearray(tw * th)
    p = 0
    for y in range(th):
        for x in range(tw):
            indices[p] = q_idx[x, y] & 0xFF
            p += 1
    transp = False                                   # opaque -> flag 'C', TransparentColor = -1

    src_ni = dict(numcols=numcols, palette=bytes(palette), indices=bytes(indices))

    im1 = build_im_strings(0, palette, indices, tw, th, numcols, transp)
    im2 = build_im_strings(1, palette, indices, tw, th, numcols, transp)
    tooltypes = [NI_SPACE, NI_GUARD] + im1 + im2
    print(f"NewIcons: {numcols}-colour palette, IM1 lines={len(im1)} IM2 lines={len(im2)} "
          f"(total tooltypes {len(tooltypes)})")

    # ---- self round-trip BEFORE writing (sanity) ----
    chk = decode_newicon(tooltypes, 0)
    assert chk["palette"] == src_ni["palette"] and chk["indices"] == src_ni["indices"], \
        "pre-write NewIcons round-trip FAILED"

    # ---- assemble .info ----
    diskobj = build_diskobject(tw, th, has_tooltypes=True)
    image1 = build_image(tw, th, 2, planes)
    blob = diskobj + image1 + serialize_tooltypes(tooltypes)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"wrote: {args.out}  ({len(blob)} bytes)")

    # ---- verify by re-reading and decoding through the AROS DecodeNI port ----
    ok, summary, ni1 = verify(args.out, tw, th, src_ni)
    print("\n=== VERIFY ===")
    for k, v in summary.items():
        print(f"  {k}: {v}")
    print(f"  ALL OK: {ok}")

    # ---- preview of the DECODED NewIcons image (what Workbench draws) ----
    if args.preview is None:
        args.preview = os.path.join(os.environ.get("TEMP", "."), "openttd_newicon_preview.png")
    render_newicon(ni1).save(args.preview)
    print(f"NewIcons preview (decoded, 6x): {args.preview}")

    if not ok:
        sys.exit(2)


if __name__ == "__main__":
    main()
