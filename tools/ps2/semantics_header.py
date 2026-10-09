#!/usr/bin/env python3
"""Makes the PS2 build's halo_msvc_semantics.h (port/ps2/include/halo_ps2_prefix.h):

    python tools/ps2/semantics_header.py --output FILE [--platform]

It is tools/linux_msvc_semantics.py's header (file-scope struct tags, and
`#pragma weak` on every inline function) without the `#pragma weak` of the
inline functions the source itself makes static, which GCC refuses: the
game's `static __inline` functions and the XDK's D3DINLINE ones (`static
__forceinline`). --platform makes the platform layer's header, from the XDK
declarations alone, as the Linux build does.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

XDK_INCLUDE = Path("port/include/xdk")
STATIC_INLINE = re.compile(
    r"(?:\bstatic\s+(?:__inline|_inline|__forceinline)|\bD3DINLINE)\b[^;{}()]*?\b([A-Za-z_]\w*)\s*\(")


def static_inline_functions(roots):
    names = set()
    for root in roots:
        for path in root.rglob("*"):
            if path.suffix in (".c", ".h", ".inl") and path.is_file():
                names.update(STATIC_INLINE.findall(path.read_text(encoding="latin-1")))
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--platform', action='store_true')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.platform:
        scan, roots = ["--inlines", str(XDK_INCLUDE)], [XDK_INCLUDE]
    else:
        scan = ["--all-inlines", "--tags", "source", "--inlines", "source", "--inlines", str(XDK_INCLUDE)]
        roots = [Path("source"), XDK_INCLUDE]
    subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(args.output), *scan],
                   check=True)
    static = static_inline_functions(roots)
    lines = args.output.read_text().splitlines(keepends=True)
    args.output.write_text("".join(line for line in lines
                                   if not (line.startswith("#pragma weak ") and line.split()[2] in static)))


if __name__ == '__main__':
    main()
