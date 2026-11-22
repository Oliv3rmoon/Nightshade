#!/usr/bin/env python3
# ProsperoEden - Launcher art from the source images (run by tools/launcher/assets.sh).
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes headless/prosperoeden/ui/art:

  backdrop.tga       the dusk artwork behind every screen, darkened towards the left where the
                     text sits, 2048x1152 (larger than the 1920x1080 screen: it drifts slowly)
  backdrop-blur.tga  the same picture heavily blurred and small: what frosted panels show
  brand.tga          the app icon, for the header and for games without cover art
  controller.tga     a controller in white on transparent, tinted by the home screen: 144x100,
                     twice the size it is drawn at
  nightly-backdrop.tga  Nightshade's plate (tools/launcher/art/nightshade-plate.jpg): crimson
                     nightshade flowers and plum berries at the left and right edges, a dark empty middle,
                     a crimson haze rising from the bottom; 2048x1152. Its alpha is 1 at the edges and in
                     the light, and falls to 0 in the dark middle, so the same picture is the frame laid
                     over a game's art on the home screen (over the theme's black it looks as drawn)
  nightly-backdrop-blur.tga  the plate blurred and small, for the frosted panels
  nightshade-mark.tga  Nightshade's flower (tools/launcher/art/nightshade-logo.svg) on transparent,
                     256x256: the Nightshade theme's mark in its top bar and its opening (needs
                     rsvg-convert, from librsvg)

render-art.py [name...] renders only the named pictures (backdrop, brand, controller,
                     slate-backdrop, nightly-backdrop, nightshade-mark).
"""

import io
import re
import subprocess
import sys

from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
ART = ROOT / "headless/prosperoeden/ui/art"


def save_tga(image, name):
    image.convert("RGBA").save(ART / name, orientation=1)
    print(f"{name}: {image.size[0]}x{image.size[1]}")


def backdrop(source):
    size = (2048, 1152)
    art = source.resize(size, Image.Resampling.LANCZOS)
    art = ImageEnhance.Color(art).enhance(0.82)
    art = ImageEnhance.Brightness(art).enhance(0.72)
    art = art.filter(ImageFilter.GaussianBlur(6)).convert("RGBA")
    # Dark on the left, where titles and lists sit; the picture opens to the right.
    overlay = Image.new("RGBA", size)
    draw = ImageDraw.Draw(overlay)
    stops = ((0, .97), (.26, .94), (.48, .70), (.72, .28), (1, .05))
    for x in range(size[0]):
        fraction = x / (size[0] - 1)
        for (a, first), (b, second) in zip(stops, stops[1:]):
            if fraction <= b:
                opacity = first + (second - first) * (fraction - a) / (b - a)
                break
        draw.line((x, 0, x, size[1]), fill=(6, 9, 10, round(opacity * 255)))
    art.alpha_composite(overlay)
    # And towards the bottom, under the hint line.
    fade = Image.new("RGBA", size)
    draw = ImageDraw.Draw(fade)
    for y in range(size[1]):
        opacity = max(0, (y / size[1] - .68) / .32) * .65
        draw.line((0, y, size[0], y), fill=(3, 7, 7, round(opacity * 255)))
    art.alpha_composite(fade)
    return art


def grain(image, strength):
    """Fine noise, so the dark gradients do not band on a TV."""
    noise = Image.effect_noise(image.size, 12).point(lambda value: 255 if value > 128 else 0)
    layer = Image.new("RGBA", image.size, (220, 230, 216, 0))
    layer.putalpha(noise.point(lambda value: round(value * strength)))
    result = image.copy()
    result.alpha_composite(layer)
    return result


def controller():
    """The controller as a mask: the shell is opaque, its touchpad, sticks and buttons are holes.

    Drawn in a 72x50 box (the size on screen) at 16 times that, then reduced to 144x100.
    """
    k = 16
    mask = Image.new("L", (72 * k, 50 * k), 0)
    draw = ImageDraw.Draw(mask)

    def box(x0, y0, x1, y1, radius, fill):
        draw.rounded_rectangle((x0 * k, y0 * k, x1 * k, y1 * k), radius * k, fill=fill)

    def disc(x, y, radius, fill):
        draw.ellipse(((x - radius) * k, (y - radius) * k, (x + radius) * k, (y + radius) * k), fill=fill)

    def bar(x0, y0, x1, y1, width, fill):
        draw.line((x0 * k, y0 * k, x1 * k, y1 * k), fill=fill, width=round(width * k))
        disc(x0, y0, width / 2, fill)
        disc(x1, y1, width / 2, fill)

    # The shell: a wide body and two grips that lean outwards.
    box(3.5, 5, 68.5, 34.5, 13, 255)
    bar(15.5, 25, 10, 40.5, 13.5, 255)
    bar(56.5, 25, 62, 40.5, 13.5, 255)
    # Touchpad, with the two small buttons beside it.
    box(25, 8.5, 47, 20.5, 3.2, 0)
    bar(21.6, 9.6, 21.2, 12.6, 1.5, 0)
    bar(50.4, 9.6, 50.8, 12.6, 1.5, 0)
    # Direction pad and the four buttons.
    box(9.2, 15.4, 18.2, 18.2, 1.2, 0)
    box(12.3, 12.3, 15.1, 21.3, 1.2, 0)
    for dx, dy in ((0, -3.7), (3.7, 0), (0, 3.7), (-3.7, 0)):
        disc(58.3 + dx, 16.8 + dy, 1.55, 0)
    # Sticks (a ring around a cap) and the round button between them.
    for x in (26.2, 45.8):
        disc(x, 27.6, 4.7, 0)
        disc(x, 27.6, 2.9, 255)
    disc(36, 27.4, 1.5, 0)
    mask = mask.resize((144, 100), Image.Resampling.LANCZOS)
    icon = Image.new("RGBA", mask.size, (255, 255, 255, 0))
    icon.putalpha(mask)
    return icon


def slate_backdrop():
    """A procedural dark neutral gradient: no source photo, just darkness and grain."""
    size = (2048, 1152)
    base = (16, 16, 18)
    art = Image.new("RGBA", size, (*base, 255))
    # A faint lighter band right of centre, so the backdrop is not perfectly flat.
    overlay = Image.new("RGBA", size)
    draw = ImageDraw.Draw(overlay)
    for x in range(size[0]):
        fraction = x / (size[0] - 1)
        lift = max(0.0, 1.0 - abs(fraction - 0.62) / 0.35) * 0.06
        draw.line((x, 0, x, size[1]), fill=(180, 178, 174, round(lift * 255)))
    art.alpha_composite(overlay)
    # Dark on the left (same stop pattern as Canopy, neutral colour).
    overlay = Image.new("RGBA", size)
    draw = ImageDraw.Draw(overlay)
    stops = ((0, .97), (.26, .94), (.48, .70), (.72, .28), (1, .05))
    for x in range(size[0]):
        fraction = x / (size[0] - 1)
        for (a, first), (b, second) in zip(stops, stops[1:]):
            if fraction <= b:
                opacity = first + (second - first) * (fraction - a) / (b - a)
                break
        draw.line((x, 0, x, size[1]), fill=(*base, round(opacity * 255)))
    art.alpha_composite(overlay)
    # Subtle bottom fade.
    fade = Image.new("RGBA", size)
    draw = ImageDraw.Draw(fade)
    for y in range(size[1]):
        opacity = max(0, (y / size[1] - .68) / .32) * .65
        draw.line((0, y, size[0], y), fill=(*base, round(opacity * 255)))
    art.alpha_composite(fade)
    return art


def nightly_backdrop():
    """Nightshade's plate, sized to the backdrop, with the alpha that makes it a frame too."""
    size = (2048, 1152)
    plate = Image.open(ROOT / "tools/launcher/art/nightshade-plate.jpg").convert("RGB")
    plate = plate.resize(size, Image.Resampling.LANCZOS)
    width, height = size

    def smooth(a, b, x):
        t = min(1.0, max(0.0, (x - a) / (b - a)))
        return t * t * (3.0 - 2.0 * t)

    # Opaque at the left and right edges, where the flowers are, and clear in the dark middle...
    row = [round(255 * smooth(0.16, 0.36, abs(x / (width - 1) - 0.5))) for x in range(width)]
    edge = Image.new("L", size)
    edge.putdata(row * height)
    # ... except where the middle has light of its own (the haze from the bottom, the bokeh).
    light = plate.convert("L").point(lambda v: round(255 * smooth(9, 56, v)))
    art = plate.convert("RGBA")
    art.putalpha(ImageChops.lighter(edge, light))
    return art


def nightshade_mark():
    """The flower of tools/launcher/art/nightshade-logo.svg without its square of black behind it."""
    svg = (ROOT / "tools/launcher/art/nightshade-logo.svg").read_text()
    svg = re.sub(r"\s*<rect[^>]*/>", "", svg, count=1)
    png = subprocess.run(["rsvg-convert", "-w", "256", "-h", "256", "-f", "png"], input=svg.encode(),
                         capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert("RGBA")


def main():
    ART.mkdir(parents=True, exist_ok=True)
    wanted = set(sys.argv[1:]) or {"backdrop", "brand", "controller"}
    if "backdrop" in wanted:
        source = Image.open(ROOT / "sce_sys/background-source.png").convert("RGB")
        art = backdrop(source)
        save_tga(grain(art, .025), "backdrop.tga")
        # A quarter of the size, blurred until only light and colour remain.
        small = art.resize((512, 288), Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(9))
        save_tga(small, "backdrop-blur.tga")
    if "slate-backdrop" in wanted:
        art = slate_backdrop()
        save_tga(grain(art, .02), "slate-backdrop.tga")
        small = art.resize((512, 288), Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(9))
        save_tga(small, "slate-backdrop-blur.tga")
    if "nightly-backdrop" in wanted:
        art = nightly_backdrop()
        save_tga(art, "nightly-backdrop.tga")
        # The frosted copy: the plate as it shows over the theme's black, small and blurred.
        shown = Image.new("RGBA", art.size, (11, 11, 13, 255))
        shown.alpha_composite(art)
        small = shown.resize((512, 288), Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(9))
        save_tga(small, "nightly-backdrop-blur.tga")
    if "brand" in wanted:
        icon = Image.open(ROOT / "sce_sys/icon-source.png").convert("RGBA")
        save_tga(icon.resize((384, 384), Image.Resampling.LANCZOS), "brand.tga")
    if "controller" in wanted:
        save_tga(controller(), "controller.tga")
    if "nightshade-mark" in wanted:
        save_tga(nightshade_mark(), "nightshade-mark.tga")


if __name__ == "__main__":
    main()
