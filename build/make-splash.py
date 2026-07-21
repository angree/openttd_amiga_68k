#!/usr/bin/env python3
"""PNG -> splash.dat converter for the Amiga OpenTTD startup splash.

The output format ("ASPL") is deliberately dumb: a big-endian header, an RGB
palette, then raw 8bpp palette indices. Big-endian because the 68k is
big-endian, so the Amiga loader reads the fields with no byte swapping;
palette-indexed because the driver's screen is 8bpp AGA and the splash fade is
done purely by scaling the palette registers - the pixel data is converted
chunky-to-planar exactly once and never touched again. Index 0 is forced to
pure black and is used to fill the screen around the image, so the whole
screen fades in and out together.

Everything decorative is baked in HERE, on the PC side, so the Amiga loader
never changes:
  - the four corners of the picture are rounded off (radius 8, anti-aliased
    against black, so the blend fades with the rest of the palette);
  - a credit line is rendered under the picture using OpenTTD's own normal
    UI font, taken from the real glyph sprites in the OpenGFX base set;
  - the port version is stamped into the top-right corner of the picture,
    same font, inset far enough that the rounded corner cannot clip it.
    (This was once drawn at runtime by the C side; it overran the chunky
    buffer and corrupted memory. Never move it back there - it is baked
    into the pixel data here precisely so the running game's memory is
    never touched.)

The font comes straight out of ogfx1_base.grf (GRF container v2). In the
base graphics set, sprite 2 is SPR_ASCII_SPACE (src/table/sprites.h), so
character c maps to sprite 2 + (c - 32); FS_NORMAL advances the pen by the
sprite width (src/fontcache.cpp GetGlyphWidth). Glyph pixel values are
clamped to 0..2 exactly like the game's ST_FONT loader does
(src/spriteloader/grf.cpp): 0 = transparent, 1 = text colour, 2 = shadow.

Layout (all integers big-endian):
    offset 0  : magic    4 bytes, ASCII "ASPL"
    offset 4  : uint16   width
    offset 6  : uint16   height
    offset 8  : uint16   ncolours (<= 256)
    offset 10 : uint16   reserved (0)
    offset 12 : palette  ncolours * 3 bytes, R,G,B
    then      : pixels   width * height bytes of palette indices

Usage: python3 make-splash.py [input.png] [output.dat] [ogfx1_base.grf]
                              [--version vX.Y.Z]

    --version sets the port version stamped into the picture's top-right
    corner (default: v0.4.1). Cutting a release means passing the new
    value, e.g.  python3 make-splash.py --version v0.5.0
"""

import argparse
import struct
import os

from PIL import Image, ImageDraw

CREDIT_TEXT     = "port by Grzegorz Korycki"
DEFAULT_VERSION = "v0.7.0"
CORNER_RADIUS   = 8     # px, rounded-corner radius on the picture
GAP             = 6     # px between picture and credit line
BOTTOM_PAD      = 2     # px below the credit line
VERSION_INSET_X = 8     # px from the picture's right edge to the version text
VERSION_INSET_Y = 4     # px from the picture's top edge to the version text box
TEXT_RGB        = (255, 255, 255)  # glyph value 1: the text itself
SHADOW_RGB      = (48, 48, 48)     # glyph value 2: the glyph shadow pixels


# --------------------------------------------------------------------------
# OpenTTD font glyphs from the OpenGFX base set (GRF container v2)
# --------------------------------------------------------------------------

def _grf_lz77(buf, pos, dest_size):
    """The GRF sprite (de)compression, as in src/spriteloader/grf.cpp."""
    dest = bytearray()
    n = dest_size
    while n > 0:
        code = struct.unpack_from("b", buf, pos)[0]
        pos += 1
        if code >= 0:
            size = 0x80 if code == 0 else code
            n -= size
            dest += buf[pos:pos + size]
            pos += size
        else:
            off = ((code & 7) << 8) | buf[pos]
            pos += 1
            size = -(code >> 3)
            n -= size
            for _ in range(size):
                dest.append(dest[len(dest) - off])
    return dest


def load_font_glyphs(grf_path, chars):
    """Return {char: (width, height, x_offs, y_offs, pixels)} for the normal
    UI font, decoded from a container-v2 base-set GRF. `pixels` holds the raw
    glyph values 0/1/2 (transparent / text / shadow), row-major."""
    buf = open(grf_path, "rb").read()
    if buf[:10] != b"\x00\x00GRF\x82\x0d\x0a\x1a\x0a":
        raise SystemExit("%s: not a GRF container v2 file" % grf_path)
    sprite_section = 14 + struct.unpack_from("<I", buf, 10)[0]
    if buf[14] != 0:
        raise SystemExit("%s: unsupported GRF stream compression" % grf_path)

    # Data section: sequential records; record index == base-set SpriteID.
    # A record of type 0xFD is a reference (uint32 id) into the sprite section.
    wanted = {}          # sprite-section id -> char
    pos, idx = 15, 0
    need = {2 + ord(c) - 32: c for c in set(chars)}   # SPR_ASCII_SPACE == 2
    while len(wanted) < len(need):
        num = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        if num == 0:
            raise SystemExit("%s: font sprites missing from data section" % grf_path)
        typ = buf[pos]
        pos += 1
        if idx in need:
            if typ != 0xFD:
                raise SystemExit("%s: sprite %d is not a real sprite" % (grf_path, idx))
            wanted[struct.unpack_from("<I", buf, pos)[0]] = need[idx]
        pos += num
        idx += 1

    # Sprite section: entries keyed by id; take the normal-zoom 8bpp image.
    glyphs = {}
    pos = sprite_section
    while len(glyphs) < len(wanted) and pos + 4 <= len(buf):
        sid = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        if sid == 0:
            break
        num = struct.unpack_from("<I", buf, pos)[0]
        pos += 4
        end = pos + num
        typ, zoom = buf[pos], buf[pos + 1]
        h, w, xo, yo = struct.unpack_from("<HHhh", buf, pos + 2)
        p = pos + 10
        pos = end
        if sid not in wanted or typ == 0xFF or (typ & 0x07) != 0x04 or zoom != 0:
            continue
        if typ & 0x08:
            decomp_size = struct.unpack_from("<I", buf, p)[0]
            p += 4
        else:
            decomp_size = w * h
        data = _grf_lz77(buf, p, decomp_size)
        pix = bytearray(w * h)
        if typ & 0x08:
            # chunked/tile format: per-row offset table, then chunk runs
            long_offs = decomp_size > 65535
            long_chunk = w > 256
            for y in range(h):
                o = (struct.unpack_from("<I", data, y * 4)[0] if long_offs
                     else struct.unpack_from("<H", data, y * 2)[0])
                while True:
                    if long_chunk:
                        cinfo, skip = struct.unpack_from("<HH", data, o)
                        o += 4
                        last, length = cinfo & 0x8000, cinfo & 0x7FFF
                    else:
                        cinfo, skip = data[o], data[o + 1]
                        o += 2
                        last, length = cinfo & 0x80, cinfo & 0x7F
                    pix[y * w + skip:y * w + skip + length] = data[o:o + length]
                    o += length
                    if last:
                        break
        else:
            pix[:] = data[:w * h]
        glyphs[wanted[sid]] = (w, h, xo, yo, bytes(min(v, 2) for v in pix))

    missing = set(chars) - set(glyphs)
    if missing:
        raise SystemExit("%s: no glyphs for %r" % (grf_path, sorted(missing)))
    return glyphs


def render_text(text, glyphs):
    """Render `text` the way the game's sprite font renderer does: pen
    advances by sprite width, sprite offsets applied. Returns (w, h, pixels)
    with values 0/1/2."""
    y0 = min(yo for (_, _, _, yo, _) in glyphs.values())
    y1 = max(yo + h for (_, h, _, yo, _) in glyphs.values())
    tw = sum(glyphs[c][0] for c in text)
    th = y1 - y0
    out = bytearray(tw * th)
    pen = 0
    for c in text:
        w, h, xo, yo, pix = glyphs[c]
        for y in range(h):
            for x in range(w):
                v = pix[y * w + x]
                if v:
                    out[(y + yo - y0) * tw + (pen + x + xo)] = v
        pen += w
    return tw, th, out


# --------------------------------------------------------------------------
# Splash composition
# --------------------------------------------------------------------------

def round_corners(img, radius):
    """Anti-aliased rounded corners, blended against pure black (the fade
    background), done in RGB before quantisation so the blend colours become
    ordinary palette entries."""
    ss = 4  # supersample factor for a smooth curve
    w, h = img.size
    mask = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, w * ss - 1, h * ss - 1), radius=radius * ss, fill=255)
    mask = mask.resize((w, h), Image.LANCZOS)
    black = Image.new("RGB", (w, h), (0, 0, 0))
    return Image.composite(img, black, mask)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", nargs="?", default=os.path.join(here, "..", "amiga_openttd2.png"))
    ap.add_argument("dst", nargs="?", default=os.path.join(here, "..", "splash.dat"))
    ap.add_argument("grf", nargs="?", default=os.path.join(
        here, "..", "OpenTTD", "baseset", "opengfx", "ogfx1_base.grf"))
    ap.add_argument("--version", default=DEFAULT_VERSION,
                    help="port version stamped top-right in the picture (default %(default)s)")
    args = ap.parse_args()
    src, dst, grf = args.src, args.dst, args.grf

    img = Image.open(src).convert("RGB")
    img = round_corners(img, CORNER_RADIUS)
    iw, ih = img.size

    glyphs = load_font_glyphs(grf, CREDIT_TEXT + args.version)
    tw, th, text = render_text(CREDIT_TEXT, glyphs)
    vw, vh, vtext = render_text(args.version, glyphs)

    W = max(iw, tw)
    H = ih + GAP + th + BOTTOM_PAD
    assert W <= 320 and H <= 256, "splash %dx%d will not fit a 320x256 screen" % (W, H)

    # Quantise the picture to at most 253 colours: index 0 stays pure black,
    # and two entries are reserved for the credit text and its shadow, so
    # ncolours never exceeds 256.
    q = img.quantize(colors=253)
    pixels = list(q.getdata())
    nused = max(pixels) + 1
    pal = q.getpalette()[: nused * 3]

    text_ix = nused + 1
    shadow_ix = nused + 2
    ncolours = nused + 3
    assert ncolours <= 256

    # Compose the final indexed canvas: black border, picture centred at the
    # top (indices shifted by one), credit line centred under it.
    canvas = bytearray(W * H)                     # all index 0: black
    ix0 = (W - iw) // 2
    for y in range(ih):
        row = y * iw
        crow = y * W + ix0
        for x in range(iw):
            canvas[crow + x] = pixels[row + x] + 1
    tx0 = (W - tw) // 2
    ty0 = ih + GAP
    for y in range(th):
        for x in range(tw):
            v = text[y * tw + x]
            if v:
                canvas[(ty0 + y) * W + tx0 + x] = text_ix if v == 1 else shadow_ix

    # Version stamp, top-right INSIDE the picture. The inset keeps it clear
    # of the radius-8 rounded corner: its rightmost pixel is CORNER_RADIUS
    # away from the picture edge, so it never touches the corner arc. Baked
    # into the pixel data on purpose - drawing this at runtime once overran
    # the chunky buffer and corrupted game memory.
    vx0 = ix0 + iw - VERSION_INSET_X - vw
    vy0 = VERSION_INSET_Y
    assert vx0 >= ix0 and vy0 + vh <= ih, "version text does not fit the picture"
    for y in range(vh):
        for x in range(vw):
            v = vtext[y * vw + x]
            if v:
                canvas[(vy0 + y) * W + vx0 + x] = text_ix if v == 1 else shadow_ix

    out = bytearray()
    out += struct.pack(">4sHHHH", b"ASPL", W, H, ncolours, 0)
    out += bytes((0, 0, 0))                       # index 0: pure black
    out += bytes(pal)                             # indices 1..nused
    out += bytes(TEXT_RGB)                        # credit text
    out += bytes(SHADOW_RGB)                      # credit text shadow
    out += bytes(canvas)

    with open(dst, "wb") as f:
        f.write(out)

    print("wrote %s: %dx%d, %d colours, %d bytes (picture %dx%d, credit %dx%d, "
          "version %r %dx%d at %d,%d)"
          % (dst, W, H, ncolours, len(out), iw, ih, tw, th,
             args.version, vw, vh, vx0, vy0))


if __name__ == "__main__":
    main()
