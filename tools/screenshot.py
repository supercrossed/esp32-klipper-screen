"""
Grab a screenshot from a running display and save it as PNG.

    1. On the device's serial console run:  shot
    2. python tools/screenshot.py <device-ip> out.png

Standard library only (the PNG is written with zlib).
"""
import struct, sys, urllib.request, zlib


def bmp_to_png(bmp: bytes) -> bytes:
    if bmp[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", bmp, 10)[0]
    w, h = struct.unpack_from("<ii", bmp, 18)
    bpp = struct.unpack_from("<H", bmp, 28)[0]
    if bpp != 24:
        raise ValueError(f"unsupported bpp {bpp}")
    row = (w * 3 + 3) & ~3
    bottom_up = h > 0
    h = abs(h)
    raw = bytearray()
    for y in range(h):
        src = h - 1 - y if bottom_up else y
        line = bmp[off + src * row: off + src * row + w * 3]
        raw.append(0)  # filter: none
        rgb = bytearray(line)
        rgb[0::3], rgb[2::3] = line[2::3], line[0::3]  # BGR -> RGB
        raw += rgb

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    url = f"http://{sys.argv[1]}:8080/screenshot.bmp"
    bmp = urllib.request.urlopen(url, timeout=30).read()
    png = bmp_to_png(bmp)
    open(sys.argv[2], "wb").write(png)
    print(f"{sys.argv[2]}: {len(png) // 1024} KB")


if __name__ == "__main__":
    main()
