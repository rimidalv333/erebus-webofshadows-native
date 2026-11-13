#!/usr/bin/env python3
"""WebOfShadowsRecomp for the PS5 - PlayStation button prompts for the texture mods.

    ps_buttons.py OUT_DIR [PREVIEW_DIR]

Draws PlayStation versions of the game's four Xbox 360 button-prompt sheets and
writes them as OUT_DIR/<fingerprint>.bin for <title>/mods/textures/ (see
wos_textures.py). All the art is drawn here from scratch; nothing of the game's
own textures goes into the files, so they can ship with the title. Each sheet is
drawn where the game's sheet has its icons:

    face buttons   X -> square, Y -> triangle, A -> cross, B -> circle
    bumpers        LB -> L1, RB -> R1
    triggers       LT -> L2, RT -> R2
    sticks         LS -> L3, RS -> R3, d-pad, Start -> OPTIONS, Back -> touch pad

SPDX-License-Identifier: MIT
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wos_textures as tex  # noqa: E402

# The game's sheets: fingerprint and dump name (format, size, layout, mips)
SHEETS = {
    'face': 'fce657b5dab946f0_f20_256x256_t_e1_p8_m3_98304.bin',
    'bumpers': '1bc1a9ad3707075f_f20_256x256_t_e1_p8_m2_81920.bin',
    'triggers': 'ee16d1066be2cf83_f20_256x256_t_e1_p8_m2_81920.bin',
    'sticks': 'a3bd42c852159375_f20_256x256_t_e1_p8_m2_81920.bin',
}

S = 4  # drawn at 4x, then scaled down
RING = (255, 255, 255, 255)
BODY = (30, 31, 38, 255)
BODY_LIGHT = (58, 60, 72, 255)
TEXT = (240, 240, 244, 255)
SQUARE = (232, 138, 200, 255)
TRIANGLE = (64, 214, 170, 255)
CROSS = (124, 166, 255, 255)
CIRCLE = (255, 96, 96, 255)
FONT = None


def font(px):
    for path in (FONT, '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
                 '/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf', 'DejaVuSans-Bold.ttf'):
        if path and os.path.exists(path):
            return ImageFont.truetype(path, px * S)
    return ImageFont.load_default(px * S)


def box(b):
    return [v * S for v in b]


def body(d, b, radius, ring=5):
    """a dark rounded shape with a white ring, b = (x0, y0, x1, y1) in texels"""
    x0, y0, x1, y1 = box(b)
    d.rounded_rectangle([x0, y0, x1, y1], radius * S, fill=RING)
    r = ring * S
    d.rounded_rectangle([x0 + r, y0 + r, x1 - r, y1 - r], max(radius - ring, 0) * S, fill=BODY)
    # a soft highlight on the upper part
    hl = Image.new('RGBA', d.im.size, (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.rounded_rectangle([x0 + r, y0 + r, x1 - r, (y0 + y1) // 2], max(radius - ring, 0) * S,
                         fill=(255, 255, 255, 18))
    return hl


def label(d, b, text, px, fill=TEXT, spacing=0):
    x0, y0, x1, y1 = box(b)
    f = font(px)
    d.multiline_text(((x0 + x1) / 2, (y0 + y1) / 2), text, font=f, fill=fill, anchor='mm',
                     align='center', spacing=spacing * S)


def face_sheet():
    img = Image.new('RGBA', (256 * S, 256 * S), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    cells = {'square': (2, 2, 126, 126), 'triangle': (130, 2, 254, 126),
             'cross': (2, 130, 126, 254), 'circle': (130, 130, 254, 254)}
    for name, b in cells.items():
        body(d, b, 62, 6)
        cx, cy = (b[0] + b[2]) / 2 * S, (b[1] + b[3]) / 2 * S
        w = 12 * S
        if name == 'square':
            h = 27 * S
            d.rectangle([cx - h, cy - h, cx + h, cy + h], outline=SQUARE, width=w)
        elif name == 'triangle':
            r = 36 * S
            pts = [(cx, cy - r), (cx + r * 0.866, cy + r * 0.5), (cx - r * 0.866, cy + r * 0.5)]
            d.line(pts + [pts[0], pts[1]], fill=TRIANGLE, width=w, joint='curve')
        elif name == 'cross':
            h = 27 * S
            d.line([(cx - h, cy - h), (cx + h, cy + h)], fill=CROSS, width=w)
            d.line([(cx - h, cy + h), (cx + h, cy - h)], fill=CROSS, width=w)
        else:
            r = 31 * S
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=CIRCLE, width=w)
    return img


def bumper_sheet():
    img = Image.new('RGBA', (256 * S, 256 * S), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    for b, text in (((18, 12, 238, 116), 'L1'), ((18, 140, 238, 244), 'R1')):
        img.alpha_composite(body(d, b, 40, 6))
        d = ImageDraw.Draw(img)
        label(d, b, text, 70)
    return img


def trigger_sheet():
    img = Image.new('RGBA', (256 * S, 256 * S), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    for b, text in (((3, 39, 125, 217), 'L\n2'), ((131, 39, 253, 217), 'R\n2')):
        img.alpha_composite(body(d, b, 30, 6))
        d = ImageDraw.Draw(img)
        label(d, b, text, 66, spacing=2)
    return img


def stick_sheet():
    img = Image.new('RGBA', (256 * S, 256 * S), (255, 255, 255, 0))
    d = ImageDraw.Draw(img)
    for b, text in (((2, 2, 126, 126), 'L3'), ((130, 2, 254, 126), 'R3')):
        img.alpha_composite(body(d, b, 62, 6))
        d = ImageDraw.Draw(img)
        label(d, b, text, 50)
    # d-pad: four separate arrow-shaped pads, PlayStation style
    b = (2, 130, 126, 254)
    img.alpha_composite(body(d, b, 62, 6))
    d = ImageDraw.Draw(img)
    cx, cy = (b[0] + b[2]) / 2 * S, (b[1] + b[3]) / 2 * S
    for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
        # a pad pointing to the centre: square end outside, point inside
        n, t = 10 * S, 14 * S       # point distance from the centre, half width
        far = 44 * S
        px, py = -dy, dx            # perpendicular
        pts = [(cx + dx * n, cy + dy * n),
               (cx + dx * (n + t) + px * t, cy + dy * (n + t) + py * t),
               (cx + dx * far + px * t, cy + dy * far + py * t),
               (cx + dx * far - px * t, cy + dy * far - py * t),
               (cx + dx * (n + t) - px * t, cy + dy * (n + t) - py * t)]
        d.polygon(pts, fill=TEXT)
    # Start -> OPTIONS (three lines), Back -> touch pad
    for b, kind in (((152, 130, 232, 190), 'options'), ((152, 194, 232, 254), 'touchpad')):
        img.alpha_composite(body(d, b, 28, 5))
        d = ImageDraw.Draw(img)
        cx, cy = (b[0] + b[2]) / 2 * S, (b[1] + b[3]) / 2 * S
        if kind == 'options':
            for i in (-1, 0, 1):
                y = cy + i * 10 * S
                d.line([(cx - 16 * S, y), (cx + 16 * S, y)], fill=TEXT, width=5 * S)
        else:
            d.rounded_rectangle([cx - 22 * S, cy - 13 * S, cx + 22 * S, cy + 13 * S], 5 * S,
                                outline=TEXT, width=5 * S)
    return img


DRAW = {'face': face_sheet, 'bumpers': bumper_sheet, 'triggers': trigger_sheet,
        'sticks': stick_sheet}


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    out_dir = sys.argv[1]
    preview = sys.argv[2] if len(sys.argv) == 3 else None
    os.makedirs(out_dir, exist_ok=True)
    for name, dump in SHEETS.items():
        info = tex.parse(dump)
        img = DRAW[name]().resize((info['width'], info['height']), Image.LANCZOS)
        if preview:
            os.makedirs(preview, exist_ok=True)
            img.save(os.path.join(preview, f'{name}.png'))
        data = bytearray(info['size'])
        tex.encode_levels(info, img, data)
        path = os.path.join(out_dir, info['hash'] + '.bin')
        open(path, 'wb').write(tex.swap(bytes(data), info['endian']))
        print(f'{name}: {path}')


if __name__ == '__main__':
    main()
