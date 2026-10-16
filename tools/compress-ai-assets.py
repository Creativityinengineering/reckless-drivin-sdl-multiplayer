#!/usr/bin/env python3
"""Compress AI assets (.spr, .hqa) in place, losslessly.

The pixels go through a PNG-like average filter (each byte minus the mean of
its left and upper neighbours) and then zlib; the game undoes both when
loading (src/assetfile.c). Without numpy the filter is skipped (plain zlib).
Files that are already compressed are left alone, so it is safe to run again.

    tools/compress-ai-assets.py DIR [DIR ...]
"""
import os
import struct
import sys
import zlib
from concurrent.futures import ProcessPoolExecutor

try:
    import numpy as np
except ImportError:
    np = None


def layout(path, raw):
    """Header length, row bytes and bytes per pixel of an asset, or None."""
    if path.endswith(".hqa") and raw[:4] == b"RDHQ" and len(raw) > 12:
        width = struct.unpack(">H", raw[4:6])[0] * 4
        return 12, width * 4, 4
    if path.endswith(".spr") and len(raw) > 8:
        width, height = struct.unpack(">HH", raw[:4])
        if width and height and (len(raw) - 8) % (width * height) == 0:
            bpp = (len(raw) - 8) // (width * height)
            if bpp in (2, 4):
                return 8, width * bpp, bpp
    return None


def compress(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:3] == b"RDZ":
        return len(raw), len(raw)
    shape = layout(path, raw) if np is not None else None
    if shape and (len(raw) - shape[0]) % shape[1] == 0:
        header, row_bytes, bpp = shape
        a = np.frombuffer(raw[header:], dtype=np.uint8).reshape(-1, row_bytes).astype(np.int16)
        f = a.copy()
        f[1:, bpp:] = (a[1:, bpp:] - ((a[1:, :-bpp] + a[:-1, bpp:]) >> 1)) & 255
        body = raw[:header] + f.astype(np.uint8).tobytes()
        packed = (b"RDZ2" + struct.pack(">IHIB", len(raw), header, row_bytes, bpp) +
                  zlib.compress(body, 9))
    else:
        packed = b"RDZ1" + struct.pack(">I", len(raw)) + zlib.compress(raw, 9)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(packed)
    os.replace(tmp, path)
    return len(raw), len(packed)


def main():
    paths = []
    for root in sys.argv[1:]:
        for folder, _, files in os.walk(root):
            paths += [os.path.join(folder, n) for n in files
                      if n.endswith((".spr", ".hqa"))]
    before = after = 0
    with ProcessPoolExecutor() as pool:
        for old, new in pool.map(compress, paths):
            before += old
            after += new
    print(f"{len(paths)} files: {before / 1e6:.0f} MB -> {after / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
