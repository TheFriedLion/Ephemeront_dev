"""Writes src/ephemeront.ico, the executable's icon.

Draws the same five gold "level meter" bars as MakeAppIcon in src/util.cpp (the
tray and window icon), at every size Windows asks for. Run it again after
changing that design:

    python tools/make_icon.py
"""
import os
import struct
import zlib

import numpy as np

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
HEIGHTS = [0.38, 0.72, 1.0, 0.6, 0.32]
BAR_W, GAP, MARGIN, SPAN = 0.12, 0.06, 0.08, 0.84
GOLD = (0xF2, 0xA9, 0x3B)  # RGB
SS = 4  # supersampling per axis, for antialiasing


def draw(size):
    """Returns straight-alpha BGRA rows, top-down, matching MakeAppIcon."""
    s = (np.arange(size * SS) + 0.5) / (size * SS)
    u, v = np.meshgrid(s, s)
    hit = np.zeros(u.shape, bool)
    r = BAR_W / 2
    for b, h in enumerate(HEIGHTS):
        cx = MARGIN + b * (BAR_W + GAP) + r
        half = h * SPAN / 2
        top, bottom = 0.5 - half + r, 0.5 + half - r
        dy = np.where(v < top, top - v, np.where(v > bottom, v - bottom, 0.0))
        hit |= (u - cx) ** 2 + dy ** 2 <= r * r
    alpha = hit.reshape(size, SS, size, SS).sum(axis=(1, 3)) * 255 // (SS * SS)
    img = np.zeros((size, size, 4), np.uint8)
    img[..., 0], img[..., 1], img[..., 2] = GOLD[2], GOLD[1], GOLD[0]
    img[..., 3] = alpha
    return img


def as_bmp(img):
    """Icon-style DIB: header with doubled height, bottom-up BGRA, then an empty AND mask."""
    size = img.shape[0]
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    mask_row = ((size + 31) // 32) * 4
    return header + img[::-1].tobytes() + bytes(mask_row * size)


def as_png(img):
    size = img.shape[0]
    rgba = img[..., [2, 1, 0, 3]]
    raw = b''.join(b'\0' + rgba[y].tobytes() for y in range(size))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    images = [as_png(draw(n)) if n >= 256 else as_bmp(draw(n)) for n in SIZES]
    out = struct.pack('<HHH', 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for n, data in zip(SIZES, images):
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b''.join(images)
    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'ephemeront.ico')
    with open(path, 'wb') as f:
        f.write(out)
    print('wrote %s (%d sizes, %d bytes)' % (os.path.normpath(path), len(SIZES), len(out)))


if __name__ == '__main__':
    main()
