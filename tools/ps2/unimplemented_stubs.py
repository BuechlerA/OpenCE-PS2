#!/usr/bin/env python3
"""Writes a stub for every function the PS2 build calls and nothing defines
(tools/ps2_build.py runs it before the link):

    python tools/ps2/unimplemented_stubs.py --cc GCC --output FILE.c --report FILE.txt @OBJECTS.rsp

The PS2 build is being brought up: the game links while much of its
platform layer is still to write. Each function that the objects call and
that neither they nor the libraries the EE's GCC links by default define
gets a stub that calls halo_ps2_unimplemented with its name
(port/ps2/src/ps2_unimplemented.c), which prints "unimplemented: NAME" and
stops. A run in PCSX2 (tools/ps2/pcsx2_run.py --until unimplemented:) so
names the next function to write. The report lists the stubs.

It also refuses a build whose objects define a function or variable that
the libraries do (anything but the few the game means to, OVERRIDES): the
linker would quietly take ours, and the libraries' own calls with it, as
the Win32 CreateThread once replaced the EE kernel's and stopped the
program before main.

A symbol is taken as a function when an object calls it (an R_MIPS_26
relocation, jal) or a header declares it with a parameter list. Any other
missing symbol is data, which a stub cannot stand in for: the script lists
them and fails.
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

# the libraries mips64r5900el-ps2-elf-gcc links by default (gcc -dumpspecs, *lib)
DEFAULT_LIBRARIES = ("m", "c", "cdvd", "pthread", "pthreadglue", "cglue", "kernel", "gcc")
# the library names the game defines on purpose: its MSVC string functions
# (source/cseries/cseries.c), which behave as newlib's
OVERRIDES = {"strlwr", "strupr", "strnlen"}
HEADER_ROOTS = (Path("source"), Path("port/include"), Path("port/linux/include"), Path("port/linux/src"),
                Path("port/linux/game"), Path("port/ps2"))


def tool(cc, name):
    return str(Path(cc).with_name(Path(cc).name.replace("gcc", name)))


def symbols(nm, files, strong=None):
    """(defined, {undefined: [objects]}) of the files; strong, if given,
    gets {name: object} of the definitions that are not weak"""
    defined, undefined = set(), collections.defaultdict(list)
    for i in range(0, len(files), 100):
        out = subprocess.run([nm, "-g", *files[i:i + 100]], capture_output=True, text=True, check=True).stdout
        current = None
        for line in out.splitlines():
            if line.endswith(":"):
                current = line[:-1]
                continue
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("U", "w"):
                undefined[parts[1]].append(current)
            elif len(parts) == 3 and parts[1] not in ("U", "w"):
                defined.add(parts[2])
                if strong is not None and parts[1] not in ("W", "V", "v"):
                    strong.setdefault(parts[2], current)
    return defined, undefined


def called(objdump, files, names):
    """the names an object calls with jal"""
    out = set()
    for i in range(0, len(files), 100):
        text = subprocess.run([objdump, "-r", *files[i:i + 100]], capture_output=True, text=True).stdout
        for line in text.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == "R_MIPS_26" and parts[2] in names:
                out.add(parts[2])
    return out


def declared_as_functions(names):
    found = set()
    pattern = re.compile(r"\b(" + "|".join(map(re.escape, sorted(names))) + r")\s*\(") if names else None
    if not pattern:
        return found
    for root in HEADER_ROOTS:
        for path in root.rglob("*"):
            if path.suffix in (".h", ".c") and path.is_file():
                found.update(pattern.findall(path.read_text(encoding="latin-1")))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], fromfile_prefix_chars='@')
    parser.add_argument('--cc', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('objects', nargs='+')
    args = parser.parse_args()
    # ninja quotes the paths with spaces in a response file ('source/saved games/...')
    args.objects = [o[1:-1] if len(o) > 1 and o[0] == o[-1] and o[0] in "'\"" else o for o in args.objects]

    nm, objdump = tool(args.cc, "nm"), tool(args.cc, "objdump")
    libraries = []
    for name in DEFAULT_LIBRARIES:
        path = subprocess.run([args.cc, f"-print-file-name=lib{name}.a"], capture_output=True, text=True,
                              check=True).stdout.strip()
        if Path(path).is_file():
            libraries.append(path)
    ours = {}
    defined, undefined = symbols(nm, args.objects, ours)
    library_strong = {}
    library_defined, _ = symbols(nm, libraries, library_strong)
    clashes = sorted(name for name in ours if name in library_strong and name not in OVERRIDES)
    if clashes:
        for name in clashes:
            print(f"unimplemented_stubs: {ours[name]} defines {name}, which {library_strong[name]} defines too",
                  file=sys.stderr)
        sys.exit(1)
    missing = sorted(s for s in undefined if s not in defined and s not in library_defined)

    functions = called(objdump, args.objects, set(missing))
    functions |= declared_as_functions(set(missing) - functions)
    data = [s for s in missing if s not in functions]
    if data:
        for name in data:
            print(f"unimplemented_stubs: {name} is missing data (used by {', '.join(sorted(set(undefined[name]))[:3])})",
                  file=sys.stderr)
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "/* Generated by tools/ps2/unimplemented_stubs.py: the functions the PS2 build\n"
        "calls and does not define yet. Each prints its name and stops\n"
        "(port/ps2/src/ps2_unimplemented.c). Do not edit. */\n\n"
        "void halo_ps2_unimplemented(const char *name);\n\n"
        "#define UNIMPLEMENTED(name) void name(void) { halo_ps2_unimplemented(#name); }\n\n"
        + "".join(f"UNIMPLEMENTED({name})\n" for name in missing))
    args.report.write_text(f"{len(missing)} functions stubbed\n\n" + "".join(f"{name}\n" for name in missing))
    print(f"unimplemented_stubs: {len(missing)} functions stubbed ({args.report})")


if __name__ == '__main__':
    main()
