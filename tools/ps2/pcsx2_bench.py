#!/usr/bin/env python3
"""Runs the PS2 build's benchmark (port/ps2/src/ps2_bench.c) in PCSX2: the
game loads a level from the main menu, plays it with a scripted player for a
number of ticks, and logs how long its simulation ticks take:

    python tools/ps2/pcsx2_bench.py [--map bloodgulch] [--ticks 600]

It writes the settings the benchmark needs (debug.bench_map and
debug.bench_ticks) to build/ps2/config.toml, leaving the others as they are,
and runs build/ps2/halo.elf (ninja ps2) with pcsx2_run.py until the
benchmark ends. The maps must be in build/ps2/maps (tools/ps2/README.md).

The times are the EE's cycle counter, which in PCSX2 counts emulated cycles:
they leave out the caches' and the memory's waits. Use them to compare
builds, not as the PS2's.

Exit status: 0 when the benchmark ends, otherwise pcsx2_run.py's.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / "build/ps2/halo.elf"
CONFIG = ROOT / "build/ps2/config.toml"
# a level's path, as the game names it: levels\test\<map>\<map> (multiplayer),
# levels\<map>\<map> (the campaign's and ui)
MULTIPLAYER_MAPS = ("beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation",
                    "hangemhigh", "longest", "prisoner", "putput", "ratrace", "sidewinder", "wizard")


def level_name(name):
    if "\\" in name:
        return name
    if name in MULTIPLAYER_MAPS:
        return f"levels\\test\\{name}\\{name}"
    return f"levels\\{name}\\{name}"


def set_debug_settings(text, settings):
    """text with settings (name: TOML value) set in its [debug] section, which
    it gets when it has none"""
    if not re.search(r"^\[debug\]\s*$", text, re.M):
        text += "\n[debug]\n"
    for name, value in settings.items():
        line = f"{name} = {value}"
        text, count = re.subn(rf"^{name}\s*=.*$", lambda match: line, text, count=1, flags=re.M)
        if not count:
            text = re.sub(r"^\[debug\]\s*$", lambda match: f"[debug]\n{line}", text, count=1, flags=re.M)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--map", default="bloodgulch", help="the level (default bloodgulch)")
    parser.add_argument("--ticks", type=int, default=600, help="ticks to play (30 a second; default 600)")
    parser.add_argument("--timeout", type=float, default=600, help="seconds (default 600)")
    args = parser.parse_args()

    if not ELF.is_file():
        raise SystemExit(f"{ELF}: not found (ninja ps2)")
    level = level_name(args.map).replace("\\", "\\\\")
    text = CONFIG.read_text() if CONFIG.is_file() else ""
    text = set_debug_settings(text, {"bench_map": f'"{level}"', "bench_ticks": str(args.ticks)})
    CONFIG.write_text(text)
    command = [sys.executable, str(ROOT / "tools/ps2/pcsx2_run.py"), str(ELF), "--until", "bench: done",
               "--timeout", str(args.timeout)]
    output = subprocess.run(command, capture_output=True, text=True)
    for line in output.stdout.splitlines():
        if "bench:" in line or "TLB Miss" in line:
            print(line)
    if output.returncode:
        print(output.stderr.strip() or "the benchmark did not end", file=sys.stderr)
    return output.returncode


if __name__ == "__main__":
    sys.exit(main())
