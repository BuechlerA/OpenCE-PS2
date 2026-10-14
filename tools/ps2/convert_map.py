#!/usr/bin/env python3
"""The PS2 map converter's first pass: an Xbox map as the PS2 reads it.

    python tools/ps2/convert_map.py MAP.map... [--out DIR]

The Xbox's maps are a 0x800-byte header and a zlib stream; the Xbox
decompresses each to a cache file on its hard disk before it plays it
(source/cache/cache_files_decompress_windows.c). A retail PS2 has no disk, so
the converter does that once, on the PC: its output is the map as the Xbox's
cache file holds it, a header and the inflated data, which the PS2 build
reads in place (source/cache/cache_files_windows.c, "PS2").

The header says how long the whole map is (its size, at offset 8), so a
converted map is told from one that is not by its file's length.

This pass changes no tag. The rest of the converter (docs/ps2_port_plan.md,
Phase 2) comes after it.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

HEADER_SIZE = 0x800
HEADER_SIGNATURE = b"daeh"  # 'head', little-endian
FOOTER_SIGNATURE = b"toof"  # 'foot'
SIZE_OFFSET = 8
NAME_OFFSET = 0x20


class MapError(Exception):
    pass


def convert(data: bytes) -> bytes:
    """the map as the cache file holds it, from the map as the DVD holds it
    (a converted map is returned as it is)"""
    if len(data) < HEADER_SIZE or data[:4] != HEADER_SIGNATURE or data[HEADER_SIZE - 4:HEADER_SIZE] != FOOTER_SIGNATURE:
        raise MapError("not a map: no header")
    size = struct.unpack_from("<I", data, SIZE_OFFSET)[0]
    if size == len(data):
        return data
    stream = zlib.decompressobj()
    try:
        body = stream.decompress(data[HEADER_SIZE:])
    except zlib.error as error:
        raise MapError(f"damaged: {error}") from error
    if not stream.eof:
        raise MapError("damaged: the data ends early")
    if HEADER_SIZE + len(body) != size:
        raise MapError(f"damaged: {HEADER_SIZE + len(body)} bytes, the header says {size}")
    return data[:HEADER_SIZE] + body


def map_name(data: bytes) -> str:
    return data[NAME_OFFSET:NAME_OFFSET + 0x20].split(b"\0")[0].decode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("maps", nargs="*", type=Path)
    parser.add_argument("--out", type=Path, default=Path("build/ps2/maps"), help="default build/ps2/maps")
    args = parser.parse_args()
    if not args.maps:
        parser.error("no maps")
    args.out.mkdir(parents=True, exist_ok=True)
    failed = False
    for path in args.maps:
        try:
            converted = convert(path.read_bytes())
        except MapError as error:
            print(f"{path}: {error}", file=sys.stderr)
            failed = True
            continue
        target = args.out / (map_name(converted) + ".map")
        if target.resolve() == path.resolve():
            print(f"{path}: already converted")
            continue
        target.write_bytes(converted)
        print(f"{target}: {len(converted)} bytes")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
