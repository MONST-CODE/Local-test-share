#!/usr/bin/env python3
"""Losslessly convert uncompressed 24/32-bit BMP captures to RGBA8 PNG (stdlib only)."""
import argparse
import struct
import zlib
from pathlib import Path


def chunk(kind: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(kind + data) & 0xffffffff
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', crc)


def convert(source: Path, destination: Path) -> tuple[int, int]:
    bmp = source.read_bytes()
    if len(bmp) < 54 or bmp[:2] != b'BM':
        raise ValueError('not a BMP file')
    pixel_offset = struct.unpack_from('<I', bmp, 10)[0]
    dib_size = struct.unpack_from('<I', bmp, 14)[0]
    if dib_size < 40 or len(bmp) < 14 + dib_size:
        raise ValueError('unsupported or truncated DIB header')
    width = struct.unpack_from('<i', bmp, 18)[0]
    signed_height = struct.unpack_from('<i', bmp, 22)[0]
    planes, depth = struct.unpack_from('<HH', bmp, 26)
    compression = struct.unpack_from('<I', bmp, 30)[0]
    height = abs(signed_height)
    if (width <= 0 or height <= 0 or width > 16384 or height > 16384 or
            planes != 1 or depth not in (24, 32) or compression != 0):
        raise ValueError('expected bounded, uncompressed 24/32-bit BMP')
    stride = ((width * depth + 31) // 32) * 4
    if pixel_offset + stride * height > len(bmp):
        raise ValueError('pixel array extends beyond BMP file')
    raw = bytearray()
    for y in range(height):
        row_index = y if signed_height < 0 else height - 1 - y
        row = bmp[pixel_offset + row_index * stride:pixel_offset + (row_index + 1) * stride]
        raw.append(0)  # PNG filter: None
        for x in range(width):
            i = x * (depth // 8)
            blue, green, red = row[i:i + 3]
            alpha = row[i + 3] if depth == 32 else 255
            raw.extend((red, green, blue, alpha))
    png = (b'\x89PNG\r\n\x1a\n' +
           chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(png)
    return width, height


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    width, height = convert(args.source, args.destination)
    print(f'PNG RGBA8 {width}x{height}: {args.destination}')


if __name__ == '__main__':
    main()
