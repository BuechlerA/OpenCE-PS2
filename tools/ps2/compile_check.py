#!/usr/bin/env python3
"""Compiles the game's C files for the PS2's Emotion Engine and reports what
breaks (docs/ps2_port_plan.md, Phase 1):

    python tools/ps2/compile_check.py [--jobs N] [--only PATTERN ...] [--json OUT]

Each file of the game (port/linux/port.json "game") is compiled to an object
with ps2dev's GCC (mips64r5900el-ps2-elf-gcc, found through PS2DEV), with the
Linux build's MSVC environment (port/linux/include, the XDK declarations,
halo_msvc_semantics.h) under port/ps2/include/halo_ps2_prefix.h. Nothing is
linked. The report lists the files that fail and the errors, grouped by
message, most files first, so the work can be taken in order.

The objects go to build/ps2/obj, the generated header and the report to
build/ps2.
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.android_build import VARIADIC_PROTOTYPE_FILES  # noqa: E402
from tools.linux_build import PORT_DIR, XDK_INCLUDE, _load_port_config, game_sources  # noqa: E402

BUILD = Path("build/ps2")
PREFIX = Path("port/ps2/include/halo_ps2_prefix.h")
# The files that call variadic functions without their prototype, which the
# Android build gives them (tools/android_abi_check.py finds them): the EE's
# n32 ABI passes floating-point arguments in their own registers too, so an
# unprototyped variadic call would pass them where the callee does not look.
VARIADIC_PROTOTYPES = Path("port/android/include/halo_android_variadic_prototypes.h")

# The Linux build's ABI flags (tools/linux_build.py, LINUX_ABI_FLAGS) that
# carry over: the EE's n32 ABI already aligns 8-byte members as MSVC does,
# so -malign-double has no counterpart, and -m32 and --target are clang's.
EE_FLAGS = [
    "-D_EE", "-G0",
    "-fms-extensions", "-fshort-wchar", "-fcommon", "-fno-pic",
    "-fno-strict-aliasing", "-fwrapv", "-fno-delete-null-pointer-checks",
    "-freg-struct-return", "-fno-omit-frame-pointer", "-ffp-contract=off",
]
# tools/linux_build.py GAME_FLAGS: C89 with MSVC's leniency. GCC 14 and later
# make implicit declarations, implicit int and pointer/integer mismatches
# errors; -fpermissive makes them warnings again, as -Wno-error=... does for
# clang.
GAME_FLAGS = ["-std=gnu89", "-D__STRICT_ANSI__", "-w", "-fpermissive"]

ERROR = re.compile(r'^(?P<file>[^:\n]+):(?P<line>\d+):(?:\d+:)? (?:fatal )?error: (?P<message>.*)$', re.M)


def compiler():
    ps2dev = os.environ.get("PS2DEV")
    if not ps2dev:
        raise SystemExit("PS2DEV is not set: install ps2dev (https://github.com/ps2dev/ps2dev) and set PS2DEV")
    cc = Path(ps2dev) / "ee/bin/mips64r5900el-ps2-elf-gcc"
    if not cc.exists():
        raise SystemExit(f"{cc}: not found")
    return cc


# an inline function the source itself makes static (D3DINLINE is
# `static __forceinline`, xdk_d3d8.h): GCC refuses `#pragma weak` on it
STATIC_INLINE = re.compile(
    r"(?:\bstatic\s+(?:__inline|_inline|__forceinline)|\bD3DINLINE)\b[^;{}()]*?\b([A-Za-z_]\w*)\s*\(")


def semantics_header():
    """tools/linux_msvc_semantics.py's header, without the `#pragma weak` of
    the functions the source makes static (halo_ps2_prefix.h)"""
    out = BUILD / "halo_msvc_semantics.h"
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(out), "--all-inlines",
                    "--tags", "source", "--inlines", "source", "--inlines", str(XDK_INCLUDE)], check=True)
    static = set()
    for root in (Path("source"), XDK_INCLUDE):
        for path in root.rglob("*"):
            if path.suffix in (".c", ".h", ".inl") and path.is_file():
                static.update(STATIC_INLINE.findall(path.read_text(encoding="latin-1")))
    lines = out.read_text().splitlines(keepends=True)
    out.write_text("".join(line for line in lines
                           if not (line.startswith("#pragma weak ") and line.split()[2] in static)))
    return out


def object_sizes(objects):
    """the bytes of code, constants, data and bss in the objects (size -A)"""
    size = Path(os.environ["PS2DEV"]) / "ee/bin/mips64r5900el-ps2-elf-size"
    totals = collections.Counter()
    for i in range(0, len(objects), 200):
        out = subprocess.run([str(size), "-A", *map(str, objects[i:i + 200])], capture_output=True, text=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 2 and parts[1].isdigit():
                name = parts[0]
                kind = next((k for k in ("text", "rodata", "data", "bss") if name.startswith("." + k)
                             or (k == "bss" and name in (".sbss", "COMMON", ".scommon"))), None)
                if kind:
                    totals[kind] += int(parts[1])
    return dict(totals)


def normalize(message):
    """an error message with its names and numbers taken out, to group by"""
    message = re.sub(r"'[^']*'", "'…'", message)
    message = re.sub(r'‘[^’]*’', "'…'", message)
    return re.sub(r'\b\d+\b', 'N', message)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--jobs', type=int, default=os.cpu_count())
    parser.add_argument('--optimize', default='-Os',
                        help="the optimization options, space-separated (default -Os: memory is the PS2's limit)")
    parser.add_argument('--only', nargs='*', help='compile only the files whose path contains one of these')
    parser.add_argument('--json', type=Path, default=BUILD / "compile_check.json")
    args = parser.parse_args()

    cc = compiler()
    sdk = Path(os.environ.get("PS2SDK", Path(os.environ["PS2DEV"]) / "ps2sdk"))
    config = _load_port_config()
    flags = [*EE_FLAGS, *args.optimize.split(), *GAME_FLAGS,
             "-include", str(PREFIX), "-include", str(semantics_header()),
             f"-I{PORT_DIR / 'include'}", "-iquote", config["game_sources"],
             *[f"-D{d}" for d in config["game"].get("defines", [])],
             *[f"-I{d}" for d in config["game"]["include_dirs"]],
             f"-I{sdk / 'ee/include'}", f"-I{sdk / 'common/include'}",
             "-idirafter", str(XDK_INCLUDE)]
    sources = [s for s in game_sources(config) if not args.only or any(p in s.as_posix() for p in args.only)]

    def compile_one(source):
        obj = BUILD / "obj" / source.with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        extra = ["-include", str(VARIADIC_PROTOTYPES)] if source.as_posix() in VARIADIC_PROTOTYPE_FILES else []
        r = subprocess.run([str(cc), *flags, *extra, "-c", str(source), "-o", str(obj)], capture_output=True, text=True)
        return source.as_posix(), r.returncode, r.stderr

    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(compile_one, sources))

    failed = [(s, err) for s, code, err in results if code]
    groups = collections.defaultdict(lambda: {"files": set(), "count": 0, "example": None})
    for source, err in failed:
        errors = list(ERROR.finditer(err))
        if not errors:
            errors = [None]
        for m in errors:
            message = m.group('message') if m else (err.strip().splitlines() or ['(no message)'])[-1]
            g = groups[normalize(message)]
            g["files"].add(source)
            g["count"] += 1
            if g["example"] is None:
                g["example"] = f"{m.group('file')}:{m.group('line')}: {message}" if m else message
    ordered = sorted(groups.items(), key=lambda kv: (-len(kv[1]["files"]), -kv[1]["count"]))

    print(f"{len(sources) - len(failed)} of {len(sources)} game files compile for the EE ({args.optimize}); "
          f"{len(failed)} fail.")
    sizes = object_sizes([BUILD / "obj" / Path(s).with_suffix(".o") for s, code, _ in results if not code])
    if sizes:
        print("sections of the objects: " + ", ".join(f"{k} {v / 1048576:.2f} MB" for k, v in sizes.items()))
    if ordered:
        print("\nerrors, by the number of files they stop:")
        for message, g in ordered[:40]:
            print(f"  {len(g['files']):4} files, {g['count']:5} errors: {message}")
            print(f"        e.g. {g['example']}")
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(dict(
        compiled=len(sources) - len(failed), total=len(sources),
        failed={s: err for s, err in failed},
        groups=[dict(message=m, files=sorted(g["files"]), count=g["count"], example=g["example"])
                for m, g in ordered]), indent=1))
    print(f"\nreport: {args.json}")


if __name__ == '__main__':
    main()
