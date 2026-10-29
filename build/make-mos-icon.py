#!/usr/bin/env python3
"""PNG icon for the native MorphOS build (openttd-mos.info).

    python build/make-mos-icon.py [--in <png>] [--out <info>]

MorphOS's icon.library reads a plain 32-bit PNG named <program>.info as the
program's icon, so no DiskObject has to be written: the 68k icon's source
picture (amiga_openttd2.png, the same one make-info.py turns into NewIcons) is
fitted into 64x64 - the usual MorphOS icon size - with a small "PPC" badge, so
the two programs in one drawer can be told apart at a glance. The stack does
not come from the icon either: os/unix/unix.cpp defines __stack for MorphOS.
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

S = 64


def font(size):
    for name in ("arialbd.ttf", "C:/Windows/Fonts/arialbd.ttf", "/mnt/c/Windows/Fonts/arialbd.ttf",
                 "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", default=r"I:\GITHUB\Amiga_OpenTTD\amiga_openttd2.png")
    ap.add_argument("--out", default=r"I:\GITHUB\Amiga_OpenTTD\release\openttd-morphos-ppc\openttd-mos.info")
    args = ap.parse_args()

    src = Image.open(args.src).convert("RGBA")
    src.thumbnail((S, S), Image.LANCZOS)
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(src, ((S - src.width) // 2, (S - src.height) // 2), src)

    d = ImageDraw.Draw(img)
    f = font(12)
    text = "PPC"
    box = d.textbbox((0, 0), text, font=f)
    tw, th = box[2] - box[0], box[3] - box[1]
    x1, y1 = S - tw - 6, S - th - 6
    d.rounded_rectangle((x1 - 3, y1 - 3, S - 1, S - 1), radius=4, fill=(20, 60, 160, 235),
                        outline=(255, 255, 255, 255))
    d.text((x1 - box[0], y1 - box[1]), text, font=f, fill=(255, 255, 255, 255))

    img.save(args.out, "PNG")
    print("wrote", args.out, img.size)


if __name__ == "__main__":
    main()
