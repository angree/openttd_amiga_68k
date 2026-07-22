#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make-info.py  --  Convert a PNG into a classic AmigaOS Workbench icon (.info / DiskObject)
                  with rounded corners, a dual-format payload:

  (1) OldIcon  : standard OS1.x/3.1 planar image, 2 bitplanes, using ONLY the four
                 reserved Workbench pens (0=grey, 1=black, 2=white, 3=blue), so it renders
                 correctly on ANY Workbench palette.
  (2) ColorIcon: OS3.5+ "GlowIcon" (IFF FORM ICON with FACE + IMAG chunks) appended after
                 the DiskObject as icon.library userdata, carrying the full-colour image
                 (up to 256 colours) so OS3.5 / Picasso96 shows the pretty version.

All multi-byte values are big-endian (m68k). Struct layouts modelled on intuition/intuition.h
(struct Gadget = 44 bytes, struct Image = 20 bytes) and workbench/workbench.h
(struct DiskObject on-disk = 78 bytes).

Usage:
    python build/make-info.py
    python build/make-info.py --in <png> --out <info> --width 90 --height 90 --radius 12

Target file the icon decorates:  release/openttd-amiga-68k/openttd  (a WBTOOL, no extension)
"""

import os
import sys
import struct
import argparse

from PIL import Image, ImageDraw

# ----------------------------------------------------------------------------- constants

DO_MAGIC   = 0xE310      # WB_DISKMAGIC
DO_VERSION = 1           # WB_DISKVERSION

# do_Type values
WBDISK   = 1
WBDRAWER = 2
WBTOOL   = 3
WBPROJECT= 4
WBGARBAGE= 5
WBDEVICE = 6
WBKICK   = 7

# Gadget.Flags
GFLG_GADGHCOMP  = 0x0000   # highlight by complementing (single image, always works)
GFLG_GADGHBOX   = 0x0001
GFLG_GADGHIMAGE = 0x0002
GFLG_GADGHNONE  = 0x0003
GFLG_GADGIMAGE  = 0x0004   # GadgetRender points to an Image (not a Border)

# Gadget.Activation
GACT_RELVERIFY     = 0x0001
GACT_GADGIMMEDIATE = 0x0002

# Gadget.GadgetType
GTYP_BOOLGADGET = 0x0001

NO_ICON_POSITION = -0x80000000   # 0x80000000, "let Workbench place it"

# Standard 4-colour Workbench palette (pens used by every OldIcon so it is palette-safe)
WB_PALETTE = [
    (0x95, 0x95, 0x95),   # pen 0  grey  (background -- looks transparent on the WB)
    (0x00, 0x00, 0x00),   # pen 1  black
    (0xFF, 0xFF, 0xFF),   # pen 2  white
    (0x3B, 0x67, 0xA2),   # pen 3  blue
]

# ----------------------------------------------------------------------------- helpers

def be(fmt, *vals):
    return struct.pack(">" + fmt, *vals)


def rounded_alpha(size, radius):
    """Return an 'L' mask (255 inside the rounded rect, 0 in the cut corners)."""
    w, h = size
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    return m


def pack_bitplanes(idx, w, h, depth):
    """
    Turn a 2-D array of palette indices (0..2^depth-1) into Amiga planar bitmap data.
    Returns bytes: for plane in 0..depth-1, for row in 0..h-1, bytes_per_row bytes.
    Leftmost pixel = MSB (bit 7) of the first byte. Rows padded to a 16-bit word.
    """
    bpr = ((w + 15) // 16) * 2          # bytes per row, word aligned
    out = bytearray()
    for plane in range(depth):
        for y in range(h):
            row = bytearray(bpr)
            for x in range(w):
                if (idx[y][x] >> plane) & 1:
                    row[x >> 3] |= (0x80 >> (x & 7))
            out += row
    return bytes(out), bpr


# ----------------------------------------------------------------------------- OldIcon

def build_oldicon_indices(rgb_img, keep_mask):
    """
    Floyd-Steinberg dither an RGB image onto the fixed 4-colour Workbench palette.
    Pixels where keep_mask==0 (rounded-off corners) are forced to pen 0 (grey background),
    which reads as 'transparent' against the standard Workbench backdrop.
    Returns a list-of-lists of indices 0..3.
    """
    w, h = rgb_img.size
    src = rgb_img.convert("RGB").load()
    keep = keep_mask.load()

    pal = [list(c) for c in WB_PALETTE]

    # working buffer in float
    buf = [[list(src[x, y]) for x in range(w)] for y in range(h)]
    idx = [[0] * w for _ in range(h)]

    def nearest(px):
        br, bi = 1e18, 0
        for i, (pr, pg, pb) in enumerate(pal):
            dr = px[0] - pr; dg = px[1] - pg; db = px[2] - pb
            dist = dr * dr + dg * dg + db * db
            if dist < br:
                br, bi = dist, i
        return bi

    for y in range(h):
        for x in range(w):
            if not keep[x, y]:
                idx[y][x] = 0            # background/grey in the corners
                continue
            old = buf[y][x]
            i = nearest(old)
            idx[y][x] = i
            new = pal[i]
            err = (old[0] - new[0], old[1] - new[1], old[2] - new[2])
            # distribute error (Floyd-Steinberg), only onto kept pixels
            for (nx, ny, f) in ((x + 1, y, 7 / 16), (x - 1, y + 1, 3 / 16),
                                (x, y + 1, 5 / 16), (x + 1, y + 1, 1 / 16)):
                if 0 <= nx < w and 0 <= ny < h and keep[nx, ny]:
                    b = buf[ny][nx]
                    b[0] += err[0] * f
                    b[1] += err[1] * f
                    b[2] += err[2] * f
    return idx


def oldicon_preview(idx, w, h):
    """Reconstruct an RGB PIL image from 4-colour indices for eyeballing."""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = WB_PALETTE[idx[y][x]]
    return im


# ----------------------------------------------------------------------------- ColorIcon (IFF)

def iff_chunk(cid, data):
    """Build an IFF chunk: 4-char id, big-endian LONG size, data, pad to even."""
    out = cid + be("I", len(data)) + data
    if len(data) & 1:
        out += b"\x00"
    return out


def build_coloricon(rgb_img, keep_mask, max_used_colors=255):
    """
    Build the OS3.5 IFF FORM ICON (FACE + IMAG). Uncompressed image + palette (compression
    field 0), depth = 8 bits/pixel. The rounded-off corners use a reserved transparent index.
    Returns (iff_bytes, info_dict).
    """
    w, h = rgb_img.size
    keep = keep_mask.load()

    # Quantise the picture to at most `max_used_colors` colours (leave 1 slot for transparent).
    q = rgb_img.convert("RGB").quantize(colors=max_used_colors, method=Image.MEDIANCUT)
    q_px = q.load()
    src_pal = q.getpalette()  # flat [r,g,b, ...] length up to 768

    used = max_used_colors
    trans_index = used                    # reserved transparent index (== used)
    num_colors = used + 1                 # includes the transparent slot
    depth = 8                             # 8 bits per pixel -> one byte per pixel

    # Build a full palette of num_colors entries (RGB triplets).
    palette = bytearray()
    for i in range(used):
        palette += bytes(src_pal[i * 3: i * 3 + 3])
    palette += b"\x00\x00\x00"            # transparent slot colour (never shown)

    # Image data: one byte per pixel, row-major, MSB-first bitstream == byte per pixel at depth 8.
    imgdata = bytearray(w * h)
    p = 0
    for y in range(h):
        for x in range(w):
            imgdata[p] = trans_index if not keep[x, y] else q_px[x, y]
            p += 1

    # ---- FACE chunk (6 bytes) ----
    aspect = 0x11                          # 1:1 pixel aspect ratio
    max_pal_bytes = len(palette) - 1       # maximum palette bytes across images, minus 1
    face = be("BBBBH", w - 1, h - 1, 0, aspect, max_pal_bytes)

    # ---- IMAG chunk ----
    flags = 0x01 | 0x02                    # bit0 has transparent colour, bit1 palette present
    image_compression = 0                  # 0 = none
    palette_compression = 0                # 0 = none
    imag = be("BBBBBBHH",
              trans_index & 0xFF,
              (num_colors - 1) & 0xFF,
              flags,
              image_compression,
              palette_compression,
              depth,
              (len(imgdata) - 1) & 0xFFFF,
              (len(palette) - 1) & 0xFFFF)
    imag += bytes(imgdata) + bytes(palette)

    body = b"ICON" + iff_chunk(b"FACE", face) + iff_chunk(b"IMAG", imag)
    form = b"FORM" + be("I", len(body)) + body

    info = dict(num_colors=num_colors, depth=depth, trans_index=trans_index,
                img_bytes=len(imgdata), pal_bytes=len(palette), form_bytes=len(form))
    return form, info


# ----------------------------------------------------------------------------- DiskObject

def build_diskobject(w, h, do_type=WBTOOL, has_coloricon=True):
    """Return the 78-byte on-disk DiskObject (with an embedded 44-byte Gadget)."""
    # --- struct Gadget (44 bytes) ---
    gadget = b""
    gadget += be("I", 0)                              # NextGadget
    gadget += be("hh", 0, 0)                          # LeftEdge, TopEdge
    gadget += be("hh", w, h)                          # Width, Height
    gadget += be("H", GFLG_GADGIMAGE | GFLG_GADGHCOMP)  # Flags (image + complement highlight)
    gadget += be("H", GACT_RELVERIFY | GACT_GADGIMMEDIATE)  # Activation
    gadget += be("H", GTYP_BOOLGADGET)               # GadgetType
    gadget += be("I", 1)                              # GadgetRender  (nonzero -> image present)
    gadget += be("I", 0)                              # SelectRender  (0 -> none; GHCOMP highlights)
    gadget += be("I", 0)                              # GadgetText
    gadget += be("i", 0)                              # MutualExclude
    gadget += be("I", 0)                              # SpecialInfo
    gadget += be("H", 0)                              # GadgetID
    gadget += be("I", 0)                              # UserData
    assert len(gadget) == 44, len(gadget)

    # --- DiskObject wrapper ---
    do = b""
    do += be("H", DO_MAGIC)                           # do_Magic
    do += be("H", DO_VERSION)                         # do_Version
    do += gadget                                      # do_Gadget
    do += be("B", do_type)                            # do_Type
    do += be("B", 0)                                  # pad
    do += be("I", 0)                                  # do_DefaultTool  (none)
    do += be("I", 0)                                  # do_ToolTypes    (none)
    do += be("i", NO_ICON_POSITION)                   # do_CurrentX
    do += be("i", NO_ICON_POSITION)                   # do_CurrentY
    do += be("I", 0)                                  # do_DrawerData
    do += be("I", 0)                                  # do_ToolWindow
    do += be("i", 0)                                  # do_StackSize    (0 -> default)
    assert len(do) == 78, len(do)
    return do


def build_image(w, h, depth, planedata, planepick=0x03, planeonoff=0x00):
    """Return the 20-byte struct Image header followed by the planar bitmap data."""
    img = b""
    img += be("hh", 0, 0)                             # LeftEdge, TopEdge
    img += be("hh", w, h)                             # Width, Height
    img += be("h", depth)                             # Depth
    img += be("I", 1)                                 # ImageData (nonzero marker)
    img += be("B", planepick)                         # PlanePick
    img += be("B", planeonoff)                        # PlaneOnOff
    img += be("I", 0)                                 # NextImage
    assert len(img) == 20, len(img)
    return img + planedata


# ----------------------------------------------------------------------------- verify

def verify(path, exp_w, exp_h):
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    magic, ver = struct.unpack_from(">HH", data, off); off += 4
    # Gadget
    (next_g, le, te, gw, gh, flags, act, gtype,
     grender, srender, gtext, mutx, sinfo, gid, udata) = struct.unpack_from(">IhhhhHHHIIIiIHI", data, off)
    off += 44
    do_type, pad = struct.unpack_from(">BB", data, off); off += 2
    (deftool, tooltypes, cx, cy, drawer, toolwin, stack) = struct.unpack_from(">IIiiIIi", data, off)
    off += 28
    assert off == 78, off

    # Image 1 (GadgetRender present -> grender != 0)
    (ile, ite, iw, ih, idepth, idata, ppick, ponoff, nimg) = struct.unpack_from(">hhhhhIBBI", data, off)
    off += 20
    bpr = ((iw + 15) // 16) * 2
    planelen = bpr * ih * idepth
    off += planelen

    # ColorIcon?
    coloricon = None
    if off < len(data) and data[off:off + 4] == b"FORM":
        form_sz = struct.unpack_from(">I", data, off + 4)[0]
        form_id = data[off + 8:off + 12]
        coloricon = (form_id.decode("latin1"), form_sz, len(data) - off)

    summary = {
        "file": path,
        "bytes": len(data),
        "do_Magic": hex(magic),
        "do_Version": ver,
        "gadget_W_H": (gw, gh),
        "gadget_Flags": hex(flags),
        "gadget_GadgetRender_present": grender != 0,
        "gadget_SelectRender_present": srender != 0,
        "do_Type": do_type,
        "image_W_H": (iw, ih),
        "image_Depth": idepth,
        "image_bytes_per_row": bpr,
        "image_plane_data_len": planelen,
        "image_data_offset_end": off if coloricon is None else off,
        "coloricon": coloricon,
    }

    ok = (magic == DO_MAGIC and ver == DO_VERSION and gw == exp_w and gh == exp_h and
          iw == exp_w and ih == exp_h and grender != 0 and do_type == WBTOOL and
          planelen == bpr * ih * idepth)
    return ok, summary


# ----------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src",
                    default=r"I:\GITHUB\Amiga_OpenTTD\amiga_openttd2.png")
    ap.add_argument("--out", dest="out",
                    default=r"I:\GITHUB\Amiga_OpenTTD\release\openttd-amiga-68k\openttd.info")
    ap.add_argument("--maxw", type=int, default=100)
    ap.add_argument("--maxh", type=int, default=90)
    ap.add_argument("--radius", type=int, default=12,
                    help="corner radius in pixels relative to a ~180px source")
    ap.add_argument("--preview",
                    default=None, help="path for the OldIcon 4-colour preview PNG")
    ap.add_argument("--no-coloricon", action="store_true",
                    help="emit only the OldIcon, skip the OS3.5 ColorIcon")
    args = ap.parse_args()

    src = Image.open(args.src).convert("RGB")
    sw, sh = src.size
    print(f"source: {args.src}  {sw}x{sh}")

    # rounded corner mask in SOURCE resolution (radius scaled up from the 180px reference)
    src_radius = round(args.radius * sw / 180.0)
    src_mask = rounded_alpha((sw, sh), src_radius)

    # target size, keep aspect
    scale = min(args.maxw / sw, args.maxh / sh)
    tw = max(1, int(round(sw * scale)))
    th = max(1, int(round(sh * scale)))
    print(f"target: {tw}x{th}  (radius src={src_radius}px)")

    rgb_t = src.resize((tw, th), Image.LANCZOS)
    # resize the mask, then threshold to a hard 1-bit shape (icons have no alpha blending)
    mask_t = src_mask.resize((tw, th), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)

    # ---- OldIcon ----
    idx = build_oldicon_indices(rgb_t, mask_t)
    planes, bpr = pack_bitplanes(idx, tw, th, depth=2)
    print(f"OldIcon: depth=2 bpr={bpr} plane_data={len(planes)} bytes")

    # ---- assemble ----
    diskobj = build_diskobject(tw, th, do_type=WBTOOL, has_coloricon=not args.no_coloricon)
    image1 = build_image(tw, th, 2, planes, planepick=0x03, planeonoff=0x00)

    blob = diskobj + image1

    color_note = "omitted"
    if not args.no_coloricon:
        iff, cinfo = build_coloricon(rgb_t, mask_t)
        blob += iff
        color_note = (f"included (FORM ICON {cinfo['form_bytes']}B, "
                      f"{cinfo['num_colors']} colours, depth {cinfo['depth']}, "
                      f"transp idx {cinfo['trans_index']})")
        print(f"ColorIcon: {color_note}")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(blob)
    print(f"wrote: {args.out}  ({len(blob)} bytes)")

    # ---- preview ----
    if args.preview is None:
        scratch = os.environ.get("TEMP", ".")
        args.preview = os.path.join(scratch, "openttd_oldicon_preview.png")
    prev = oldicon_preview(idx, tw, th)
    prev.resize((tw * 4, th * 4), Image.NEAREST).save(args.preview)
    print(f"OldIcon preview (4x): {args.preview}")

    # ---- verify ----
    ok, summary = verify(args.out, tw, th)
    print("\n=== VERIFY ===")
    for k, v in summary.items():
        print(f"  {k}: {v}")
    print(f"  self-consistent: {ok}")
    print(f"  coloricon: {color_note}")
    if not ok:
        sys.exit(2)


if __name__ == "__main__":
    main()
