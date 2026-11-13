#!/usr/bin/env python3
"""WebOfShadowsRecomp for the PS5 - the game's texture dumps to PNG, and PNGs back.

With wos_texture_dump = true in wos_recomp.toml, the title writes each texture the
game loads to <title>/mods/dump/ as it sits in the Xbox 360's memory (tiled,
big-endian, often DXT-compressed), named

    <fingerprint>_f<format>_<width>x<height>_<t|l>_e<endian>_p<pitch/32>_m<mips>_<bytes>.bin

and uses <title>/mods/textures/<fingerprint>.bin, the same size, in place of the
texture whose data has that fingerprint (wos/patches/rexglue/0008).

    wos_textures.py topng DUMP_DIR PNG_DIR        every dump to a PNG (base level), same name
    wos_textures.py tobin DUMP.bin EDITED.png OUT_DIR
                                                  the PNG in the dump's format, as
                                                  OUT_DIR/<fingerprint>.bin for mods/textures

Formats: DXT1 (18), DXT2/3 (19), DXT4/5 (20), 8888 (6, read as BGRA), 565 (4),
4444 (15), 8 (2). topng shows the base level. tobin writes the base level and, for
a tiled power-of-two texture, the smaller mip levels the dump holds (scaled down
from the PNG), down to 32 texels; smaller levels sit in the packed mip tail and
are kept from the original. Needs Python 3 with numpy and Pillow.

SPDX-License-Identifier: MIT
"""
import os
import re
import sys

import numpy as np
from PIL import Image

NAME = re.compile(r'^([0-9a-f]{16})_f(\d+)_(\d+)x(\d+)_([tl])_e(\d)_p(\d+)_m(\d+)_(\d+)\.bin$')
# format: (block width/height, bytes per block)
FORMATS = {18: (4, 8), 19: (4, 16), 20: (4, 16), 6: (1, 4), 4: (1, 2), 15: (1, 2), 2: (1, 1)}


def parse(path):
    m = NAME.match(os.path.basename(path))
    if not m:
        raise SystemExit(f'{path}: not a texture dump name')
    h, f, w, hh, t, e, p, mips, size = m.groups()
    return dict(hash=h, format=int(f), width=int(w), height=int(hh), tiled=t == 't',
                endian=int(e), pitch=int(p), mips=int(mips), size=int(size))


def swap(data, endian):
    a = np.frombuffer(data, dtype=np.uint8).copy()
    if endian == 1:  # 8 in 16
        a = a.reshape(-1, 2)[:, ::-1].reshape(-1)
    elif endian == 2:  # 8 in 32
        a = a[: len(a) // 4 * 4].reshape(-1, 4)[:, ::-1].reshape(-1)
    elif endian == 3:  # 16 in 32
        a = a[: len(a) // 4 * 4].reshape(-1, 2, 2)[:, ::-1, :].reshape(-1)
    return a.tobytes()


def tiled_offset(x, y, pitch, bpb_log2):
    # texture_util::GetTiledOffset2D (XGAddress2DTiledOffset)
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpb_log2 + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))


def layout(info):
    """(blocks wide, blocks high, bytes per block, block offsets) of the base level"""
    bw, bpb = FORMATS[info['format']]
    blocks_x = (info['width'] + bw - 1) // bw
    blocks_y = (info['height'] + bw - 1) // bw
    pitch_blocks = (info['pitch'] * 32 + bw - 1) // bw
    pitch_blocks = (pitch_blocks + 31) & ~31
    bpb_log2 = bpb.bit_length() - 1
    offsets = np.zeros((blocks_y, blocks_x), dtype=np.int64)
    for y in range(blocks_y):
        for x in range(blocks_x):
            if info['tiled']:
                offsets[y, x] = tiled_offset(x, y, pitch_blocks, bpb_log2)
            else:
                row = (pitch_blocks * bpb + 255) & ~255
                offsets[y, x] = y * row + x * bpb
    return blocks_x, blocks_y, bpb, offsets


def rgb565(c):
    return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1)


def decode_dxt_color(block, transparent):
    c0, c1 = int.from_bytes(block[0:2], 'little'), int.from_bytes(block[2:4], 'little')
    p0, p1 = rgb565(np.array(c0)), rgb565(np.array(c1))
    if c0 > c1 or not transparent:
        pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
        alpha = [255] * 4
    else:
        pal = [p0, p1, (p0 + p1) // 2, np.zeros(3, int)]
        alpha = [255, 255, 255, 0]
    bits = int.from_bytes(block[4:8], 'little')
    rgba = np.zeros((4, 4, 4), np.uint8)
    for i in range(16):
        idx = (bits >> (2 * i)) & 3
        rgba[i // 4, i % 4, :3] = pal[idx]
        rgba[i // 4, i % 4, 3] = alpha[idx]
    return rgba


def decode_block(fmt, block):
    if fmt == 18:
        return decode_dxt_color(block, True)
    if fmt == 19:
        rgba = decode_dxt_color(block[8:16], False)
        a = int.from_bytes(block[0:8], 'little')
        for i in range(16):
            rgba[i // 4, i % 4, 3] = ((a >> (4 * i)) & 15) * 17
        return rgba
    if fmt == 20:
        rgba = decode_dxt_color(block[8:16], False)
        a0, a1 = block[0], block[1]
        if a0 > a1:
            pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
        else:
            pal = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
        bits = int.from_bytes(block[2:8], 'little')
        for i in range(16):
            rgba[i // 4, i % 4, 3] = pal[(bits >> (3 * i)) & 7]
        return rgba
    v = int.from_bytes(block, 'little')
    if fmt == 6:
        b, g, r, a = block[0], block[1], block[2], block[3]
        return np.array([[[r, g, b, a]]], np.uint8)
    if fmt == 4:
        return np.array([[list(rgb565(np.array(v))) + [255]]], np.uint8)
    if fmt == 15:
        return np.array([[[((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17]]], np.uint8)
    if fmt == 2:
        return np.array([[[v, v, v, 255]]], np.uint8)
    raise ValueError(fmt)


def to_png(path, out_path):
    info = parse(path)
    if info['format'] not in FORMATS:
        return False
    data = swap(open(path, 'rb').read(), info['endian'])
    bx, by, bpb, offsets = layout(info)
    bw = FORMATS[info['format']][0]
    img = np.zeros((by * bw, bx * bw, 4), np.uint8)
    for y in range(by):
        for x in range(bx):
            o = int(offsets[y, x])
            if o + bpb <= len(data):
                img[y * bw:(y + 1) * bw, x * bw:(x + 1) * bw] = decode_block(info['format'], data[o:o + bpb])
    Image.fromarray(img[: info['height'], : info['width']], 'RGBA').save(out_path)
    return True


def encode_dxt_color(px, transparent_ok):
    # px: 16x4 RGBA. End points: the colours farthest apart (simple, good for flat art)
    rgb = px[:, :3].astype(np.int32)
    opaque = px[:, 3] >= 128
    pts = rgb[opaque] if opaque.any() else rgb
    d = ((pts[:, None, :] - pts[None, :, :]) ** 2).sum(-1)
    i, j = np.unravel_index(np.argmax(d), d.shape)
    def to565(c):
        return ((int(c[0]) * 31 + 127) // 255 << 11) | ((int(c[1]) * 63 + 127) // 255 << 5) | ((int(c[2]) * 31 + 127) // 255)
    c0, c1 = to565(pts[i]), to565(pts[j])
    use_transparent = transparent_ok and not opaque.all()
    if use_transparent:
        if c0 > c1:
            c0, c1 = c1, c0
    else:
        if c0 < c1:
            c0, c1 = c1, c0
        if c0 == c1:
            c1 = c1 - 1 if c1 else c1
            if c0 == c1:
                c0 = 1
    p0, p1 = rgb565(np.array(c0)), rgb565(np.array(c1))
    if c0 > c1:
        pal = np.array([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3])
    else:
        pal = np.array([p0, p1, (p0 + p1) // 2, [0, 0, 0]])
    bits = 0
    for k in range(16):
        if use_transparent and not opaque[k]:
            idx = 3
        else:
            choices = range(3) if c0 <= c1 else range(4)
            idx = min(choices, key=lambda n: int(((pal[n] - rgb[k]) ** 2).sum()))
        bits |= idx << (2 * k)
    return c0.to_bytes(2, 'little') + c1.to_bytes(2, 'little') + bits.to_bytes(4, 'little')


def encode_block(fmt, px):
    if fmt == 18:
        return encode_dxt_color(px, True)
    if fmt == 19:
        a = 0
        for k in range(16):
            a |= (int(px[k, 3]) * 15 + 127) // 255 << (4 * k)
        return a.to_bytes(8, 'little') + encode_dxt_color(px, False)
    if fmt == 20:
        alpha = px[:, 3].astype(int)
        a0, a1 = int(alpha.max()), int(alpha.min())
        if a0 == a1:
            a1 = max(0, a0 - 1) if a0 else 0
            a0 = a1 + 1 if a0 == a1 else a0
        pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
        bits = 0
        for k in range(16):
            idx = min(range(8), key=lambda n: abs(pal[n] - int(alpha[k])))
            bits |= idx << (3 * k)
        return bytes([a0, a1]) + bits.to_bytes(6, 'little') + encode_dxt_color(px, False)
    p = px[0]
    if fmt == 6:
        return bytes([p[2], p[1], p[0], p[3]])
    if fmt == 4:
        v = ((int(p[0]) * 31 + 127) // 255 << 11) | ((int(p[1]) * 63 + 127) // 255 << 5) | ((int(p[2]) * 31 + 127) // 255)
        return v.to_bytes(2, 'little')
    if fmt == 15:
        v = ((int(p[3]) * 15 + 127) // 255 << 12) | ((int(p[0]) * 15 + 127) // 255 << 8) | \
            ((int(p[1]) * 15 + 127) // 255 << 4) | ((int(p[2]) * 15 + 127) // 255)
        return v.to_bytes(2, 'little')
    if fmt == 2:
        return bytes([int(p[:3].mean())])
    raise ValueError(fmt)


def level_size(info):
    """bytes of one tiled level: its blocks padded to 32x32-block tiles"""
    bw, bpb = FORMATS[info['format']]
    bx = ((info['width'] + bw - 1) // bw + 31) & ~31
    by = ((info['height'] + bw - 1) // bw + 31) & ~31
    return bx * by * bpb


def write_level(info, img, data, base):
    bx, by, bpb, offsets = layout(info)
    bw = FORMATS[info['format']][0]
    px = np.zeros((by * bw, bx * bw, 4), np.uint8)
    px[: info['height'], : info['width']] = np.array(img)
    for y in range(by):
        for x in range(bx):
            block = px[y * bw:(y + 1) * bw, x * bw:(x + 1) * bw].reshape(-1, 4)
            o = base + int(offsets[y, x])
            if o + bpb <= len(data):
                data[o:o + bpb] = encode_block(info['format'], block)


def encode_levels(info, img, data):
    """img (base size) into data: the base level, then the mips (see the top)"""
    write_level(info, img, data, 0)
    pow2 = lambda v: v & (v - 1) == 0
    base = len(data) if info['mips'] <= 1 else level_size(info)
    w, h, offset = info['width'], info['height'], base
    if info['tiled'] and pow2(w) and pow2(h):
        for level in range(1, info['mips']):
            w, h = max(w // 2, 1), max(h // 2, 1)
            if min(w, h) <= 16:
                break  # packed mip tail
            linfo = dict(info, width=w, height=h, pitch=(w + 31) // 32)
            if offset + level_size(linfo) > len(data):
                break
            write_level(linfo, img.resize((w, h), Image.LANCZOS), data, offset)
            offset += level_size(linfo)


def to_bin(dump_path, png_path, out_dir):
    info = parse(dump_path)
    if info['format'] not in FORMATS:
        raise SystemExit(f'format {info["format"]} is not supported')
    original = open(dump_path, 'rb').read()
    data = bytearray(swap(original, info['endian']))
    img = Image.open(png_path).convert('RGBA')
    if img.size != (info['width'], info['height']):
        print(f'note: {png_path} is {img.size[0]}x{img.size[1]}, scaled to {info["width"]}x{info["height"]}')
        img = img.resize((info['width'], info['height']), Image.LANCZOS)
    encode_levels(info, img, data)
    out = swap(bytes(data), info['endian'])
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, info['hash'] + '.bin')
    open(path, 'wb').write(out)
    print(f'{path}: {len(out)} bytes (the dump is {len(original)})')


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'topng':
        os.makedirs(sys.argv[3], exist_ok=True)
        done = skipped = 0
        for name in sorted(os.listdir(sys.argv[2])):
            if not name.endswith('.bin'):
                continue
            try:
                ok = to_png(os.path.join(sys.argv[2], name), os.path.join(sys.argv[3], name[:-4] + '.png'))
            except Exception as error:  # a format or size this does not know
                print(f'{name}: {error}')
                ok = False
            done += ok
            skipped += not ok
        print(f'{done} PNGs in {sys.argv[3]} ({skipped} dumps in formats this does not convert)')
    elif len(sys.argv) == 5 and sys.argv[1] == 'tobin':
        to_bin(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
