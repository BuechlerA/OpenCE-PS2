"""The PS2 build (ninja ps2): the game for the PlayStation 2's Emotion
Engine, with ps2dev's GCC (docs/ps2_port_plan.md).

The game's files, defines and include directories are the Linux build's
(port/linux/port.json), with its MSVC environment (port/linux/include and
the XDK declarations) under port/ps2/include/halo_ps2_prefix.h. port/ps2/port.json
says which of the Linux platform layer's files the PS2 build compiles as they
are; port/ps2/src holds the PS2's own. While the platform layer is brought
up, every function the game calls that nothing defines yet is a stub that
names itself and stops (tools/ps2/unimplemented_stubs.py).

The toolchain is found through configure.py --ps2dev or the PS2DEV
environment variable; without it, the build is left out.
"""

import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from tools.android_build import VARIADIC_PROTOTYPE_FILES
from tools.linux_build import (MUSL_MATH_DIR, PORT_DIR as LINUX_DIR, TOML_DIR, XDK_INCLUDE, ZLIB_DEFINES,
                               ZLIB_DIR, ZLIB_SOURCES, _load_port_config, game_sources, musl_math_sources)
from tools.ninja_syntax import Writer

PS2_DIR = Path("port/ps2")
PS2_CONFIG = PS2_DIR / "port.json"
# where the tag cache is: halo_port_capacity.h has it too, and says so if they differ
TAG_CACHE_ADDRESS = "0xEB0000"
PREFIX = PS2_DIR / "include" / "halo_ps2_prefix.h"
# the files that call variadic functions without their prototype, which the
# Android build gives them (tools/android_abi_check.py finds them): the EE's
# n32 ABI passes floating-point arguments in their own registers too
VARIADIC_PROTOTYPES = Path("port/android/include/halo_android_variadic_prototypes.h")

# The Linux build's ABI flags (tools/linux_build.py, LINUX_ABI_FLAGS) that
# carry over: the EE's n32 ABI already aligns 8-byte members as MSVC does, so
# -malign-double has no counterpart, and -m32 and --target are clang's.
# -G0 and the toolchain's default abicalls, as ps2sdk's own libraries are
# built (docs/ps2_port_plan.md: a small-data area barely shrinks the code).
EE_FLAGS = [
    "-D_EE", "-G0",
    "-fms-extensions", "-fshort-wchar", "-fcommon", "-fno-pic",
    "-fno-strict-aliasing", "-fwrapv", "-fno-delete-null-pointer-checks",
    "-freg-struct-return", "-fno-omit-frame-pointer", "-ffp-contract=off",
    "-g",
    # each function and variable in a section of its own, which the link
    # drops when nothing uses it (--gc-sections): the copies of the headers'
    # inline functions each unit emits, but one (halo_ps2_prefix.h), and
    # what the PS2 never calls
    "-ffunction-sections", "-fdata-sections",
]
# memory is the PS2's limit (docs/ps2_port_plan.md, "Code and constant data")
OPTIMISATION = "-Os"
# tools/linux_build.py GAME_FLAGS: C89 with MSVC's leniency. GCC 14 and later
# make implicit declarations, implicit int and pointer/integer mismatches
# errors; -fpermissive makes them warnings again, as -Wno-error=... does for
# clang.
GAME_FLAGS = ["-std=gnu89", "-D__STRICT_ANSI__", "-w", "-fpermissive"]
# tools/linux_build.py PLATFORM_FLAGS, less clang's warning options
PLATFORM_FLAGS = ["-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", "-fpermissive"]


def ps2dev(sln: Any) -> Optional[Path]:
    """the ps2dev installation, or None"""
    root = getattr(sln, "ps2dev", None) or os.environ.get("PS2DEV")
    if root and (Path(root) / "ee/bin/mips64r5900el-ps2-elf-gcc").exists():
        return Path(root)
    return None


def _load_config() -> Dict[str, Any]:
    with open(PS2_CONFIG, "r", encoding="utf-8") as f:
        return json.load(f)


def ps2_configure_inputs() -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [Path(__file__), PS2_CONFIG, PS2_DIR / "src", LINUX_DIR / "game"]


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def game_compile_flags(sdk: Path, semantics_header: Path) -> List[str]:
    """the flags of the game's files and of the port's game units (without
    the optimisation, which the caller chooses)"""
    config = _load_port_config()
    return [*EE_FLAGS, *GAME_FLAGS,
            "-include", str(PREFIX), "-include", str(semantics_header),
            f"-I{LINUX_DIR / 'include'}", "-iquote", config["game_sources"],
            *[f"-D{d}" for d in config["game"].get("defines", [])],
            *[f"-I{d}" for d in config["game"]["include_dirs"]],
            f"-I{sdk / 'ee/include'}", f"-I{sdk / 'common/include'}",
            "-idirafter", str(XDK_INCLUDE)]


def ps2_game_units() -> List[Path]:
    """the port's game units (port/linux/game) the PS2 build compiles"""
    excluded = set(_load_config()["port_game_units"]["exclude"])
    config = _load_port_config()
    return [s for s in sorted(Path(config["game_sources"]).glob("*.c")) if s.name not in excluded]


def generate_ps2_build(n: Writer, sln: Any) -> None:
    root = ps2dev(sln)
    if root is None or not PS2_CONFIG.is_file():
        n.comment("PS2 build (ninja ps2) left out: no ps2dev toolchain (configure.py --ps2dev, or PS2DEV)")
        n.newline()
        return
    config = _load_config()
    sdk = Path(os.environ.get("PS2SDK", root / "ps2sdk"))
    cc = root / "ee/bin/mips64r5900el-ps2-elf-gcc"
    build_dir: Path = sln.build_dir / "ps2"
    obj_dir = build_dir / "obj"
    semantics_header = build_dir / "halo_msvc_semantics.h"
    platform_semantics_header = build_dir / "platform_msvc_semantics.h"
    stubs = build_dir / "generated" / "unimplemented.c"
    output = build_dir / "halo.elf"
    release = ["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []

    n.comment("PS2 build (ninja ps2)")
    n.variable("ps2_cc", _quote(cc))
    n.rule(name="ps2_semantics", command="$python tools/ps2/semantics_header.py --output $out $kind",
           description="PS2 MSVC SEMANTICS $out", restat=True)
    n.rule(name="ps2_cc", command="$ps2_cc -MMD -MF $out.d $cflags -c $in -o $out",
           description="PS2 CC $out", depfile="$out.d", deps="gcc")
    n.rule(name="ps2_stubs",
           command="$python tools/ps2/unimplemented_stubs.py --cc $ps2_cc --output $out --report $report @$out.rsp",
           description="PS2 STUBS $out", rspfile="$out.rsp", rspfile_content="$in_newline")
    n.rule(name="ps2_link", command="$ps2_cc $ldflags -o $out @$out.rsp",
           description="PS2 LINK $out", rspfile="$out.rsp", rspfile_content="$in_newline")

    game_headers = sorted(p for p in Path("source").rglob("*") if p.suffix in (".c", ".h"))
    xdk = sorted(XDK_INCLUDE.glob("*.h"))
    n.build(outputs=semantics_header, rule="ps2_semantics", variables={"kind": ""},
            implicit=[Path("tools/ps2/semantics_header.py"), Path("tools/linux_msvc_semantics.py"), *xdk,
                      *game_headers])
    n.build(outputs=platform_semantics_header, rule="ps2_semantics", variables={"kind": "--platform"},
            implicit=[Path("tools/ps2/semantics_header.py"), Path("tools/linux_msvc_semantics.py"), *xdk])

    objects: List[Path] = []

    def add_object(source: Path, cflags: List[str]) -> None:
        obj = obj_dir / source.with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="ps2_cc", inputs=source,
                implicit=[PREFIX, semantics_header, platform_semantics_header],
                variables={"cflags": " ".join(_quote(f) if " " in f else f for f in cflags)})

    game = [*game_compile_flags(sdk, semantics_header), OPTIMISATION, *release]
    for source in game_sources(_load_port_config()):
        extra = ["-include", str(VARIADIC_PROTOTYPES)] if source.as_posix() in VARIADIC_PROTOTYPE_FILES else []
        add_object(source, game + extra)
    for source in ps2_game_units():
        add_object(source, game)

    platform = [*EE_FLAGS, *PLATFORM_FLAGS, OPTIMISATION, *release,
                "-include", str(PREFIX), "-include", str(platform_semantics_header),
                f"-I{PS2_DIR / 'src'}", f"-I{LINUX_DIR / 'src'}", f"-I{LINUX_DIR / 'include'}",
                "-Isource", "-Isource/cseries", f"-I{TOML_DIR}",
                f"-I{sdk / 'ee/include'}", f"-I{sdk / 'common/include'}", "-idirafter", str(XDK_INCLUDE)]
    # the posix_* files talk to the C library only, with its own headers (as
    # the Linux build has them, tools/linux_build.py)
    posix = [*EE_FLAGS, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_PS2", "-w", OPTIMISATION, *release,
             f"-I{sdk / 'ee/include'}", f"-I{sdk / 'common/include'}"]
    for name in config["linux_platform_sources"]["files"]:
        add_object(LINUX_DIR / "src" / name, posix if name.startswith("posix_") else platform)
    for source in sorted(Path(config["platform_sources"]).glob("*.c")):
        add_object(source, platform)

    # the same maths, zlib and settings file parser as the other builds
    # (tools/linux_build.py)
    add_object(TOML_DIR / "tomlc17.c", [*EE_FLAGS, "-std=gnu11", "-w", OPTIMISATION])
    for name in ZLIB_SOURCES:
        add_object(ZLIB_DIR / name, [*EE_FLAGS, "-std=gnu11", *ZLIB_DEFINES, "-w", OPTIMISATION])
    for source in musl_math_sources():
        add_object(source, [*EE_FLAGS, "-std=gnu11", "-w", OPTIMISATION, f"-I{MUSL_MATH_DIR}/include",
                            "-include", str(MUSL_MATH_DIR / "include/libm.h")])

    stub_report = build_dir / "unimplemented.txt"
    n.build(outputs=stubs, rule="ps2_stubs", inputs=objects,
            implicit=[Path("tools/ps2/unimplemented_stubs.py")], implicit_outputs=[stub_report],
            variables={"report": stub_report})
    stub_object = obj_dir / "generated" / "unimplemented.o"
    n.build(outputs=stub_object, rule="ps2_cc", inputs=stubs,
            variables={"cflags": " ".join([*EE_FLAGS, "-std=gnu11", "-w", OPTIMISATION])})

    # ps2sdk's link file, with a heap that ends where the tag cache is
    # (HALO_PORT_TAG_CACHE_ADDRESS, halo_port_capacity.h), which is the same in
    # every build: its _heap_size is -1, up to the thread stack (--defsym does not
    # take _end, a symbol of the link file)
    linkfile = (sdk / "ee/startup/linkfile").read_text()
    if "PROVIDE(_heap_size = -1);" not in linkfile:
        raise SystemExit(f"{sdk / 'ee/startup/linkfile'}: no _heap_size to set")
    heap_linkfile = build_dir / "linkfile"
    heap_linkfile.parent.mkdir(parents=True, exist_ok=True)
    heap_linkfile.write_text(linkfile.replace("PROVIDE(_heap_size = -1);",
                                              f"PROVIDE(_heap_size = {TAG_CACHE_ADDRESS} - _end);"))
    ldflags = [f"-T{heap_linkfile}", OPTIMISATION, f"-L{sdk / 'ee/lib'}", "-Wl,-zmax-page-size=128",
               "-Wl,--gc-sections"]
    n.build(outputs=output, rule="ps2_link", inputs=[*objects, stub_object],
            variables={"ldflags": " ".join(ldflags)})
    n.build(outputs="ps2", rule="phony", inputs=[output])
    n.newline()
