"""Generates src/app/recordion.ico: a red record dot inside a light ring.

Pure standard library (zlib + struct) so it runs anywhere: python scripts/make_icon.py
"""
import struct
import zlib
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SUPERSAMPLE = 4
RING = (235, 235, 240)
DOT = (230, 40, 50)


def render(size: int) -> bytes:
    n = size * SUPERSAMPLE
    c = (n - 1) / 2
    outer, inner, dot = n * 0.48, n * 0.38, n * 0.27
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    px, py = x * SUPERSAMPLE + sx, y * SUPERSAMPLE + sy
                    d = ((px - c) ** 2 + (py - c) ** 2) ** 0.5
                    color = DOT if d <= dot else RING if inner <= d <= outer else None
                    if color:
                        acc[0] += color[0]
                        acc[1] += color[1]
                        acc[2] += color[2]
                        acc[3] += 255
            samples = SUPERSAMPLE * SUPERSAMPLE
            a = acc[3] // samples
            if a:
                covered = acc[3] // 255
                row += bytes((acc[0] // covered, acc[1] // covered, acc[2] // covered, a))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return png(size, rows)


def png(size: int, rows: list[bytes]) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    raw = b"".join(b"\0" + r for r in rows)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main() -> None:
    images = [render(s) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for size, data in zip(SIZES, images):
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = Path(__file__).resolve().parent.parent / "src" / "app" / "recordion.ico"
    out.write_bytes(header + entries + b"".join(images))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
