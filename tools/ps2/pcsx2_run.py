#!/usr/bin/env python3
"""Boots a PS2 program in PCSX2 without its window and prints what the
program writes to the EE and IOP consoles (printf), for automated checks:

    python tools/ps2/pcsx2_run.py PROGRAM.elf [--until TEXT] [--timeout SECONDS]

PCSX2 runs with a copy of your own configuration (and so your BIOS) in a
temporary folder, with its console logging turned on; your settings are not
changed. The program's folder is PCSX2's host: device, so the program can
read files beside it as host:name.

PCSX2's log mixes the emulator's messages with the program's. What this
prints is everything the log gets after PCSX2 has loaded the program, with
the timestamps taken out.

Exit status: 0 when --until's text appears (or, without --until, when the
timeout ends with PCSX2 still running), 1 when the timeout ends before the
text appears, 2 when PCSX2 stops by itself or cannot start.
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# PCSX2's line once the program starts ("ELF host:x.elf with entry point at 0x... is executing.")
LOADED = re.compile(r'^\[\s*[\d.]+\] ELF .* is executing\.')
TIMESTAMP = re.compile(r'^\[\s*[\d.]+\] ?')


def default_pcsx2():
    if os.environ.get("PCSX2"):
        return Path(os.environ["PCSX2"])
    if platform.system() == "Darwin":
        return Path("/Applications/PCSX2.app/Contents/MacOS/PCSX2")
    return Path(shutil.which("pcsx2-qt") or shutil.which("pcsx2") or "pcsx2-qt")


def default_config():
    if platform.system() == "Darwin":
        return Path.home() / "Library/Application Support/PCSX2/inis/PCSX2.ini"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "PCSX2/inis/PCSX2.ini"


def logging_config(text):
    """the configuration with the console logging on"""
    for key in ("EnableFileLogging", "EnableEEConsole", "EnableIOPConsole"):
        text, n = re.subn(rf'^{key}\s*=.*$', f'{key} = true', text, flags=re.M)
        if not n:
            text = re.sub(r'^\[Logging\]\s*$', f'[Logging]\n{key} = true', text, count=1, flags=re.M)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('elf', type=Path)
    parser.add_argument('--until', help='stop once a line of the output contains this text')
    parser.add_argument('--timeout', type=float, default=30, help='seconds (default 30)')
    parser.add_argument('--pcsx2', type=Path, default=default_pcsx2(), help='the PCSX2 executable')
    parser.add_argument('--config', type=Path, default=default_config(),
                        help='the PCSX2.ini to copy (default: your own)')
    parser.add_argument('--log', type=Path, help="also keep PCSX2's whole log here")
    args = parser.parse_args()

    elf = args.elf.resolve()
    if not elf.is_file():
        raise SystemExit(f'{elf}: not found')
    if not args.pcsx2.exists():
        raise SystemExit(f'{args.pcsx2}: PCSX2 not found (set --pcsx2 or PCSX2)')
    if not args.config.is_file():
        raise SystemExit(f'{args.config}: no PCSX2 configuration; start PCSX2 once and choose a BIOS')

    with tempfile.TemporaryDirectory(prefix='pcsx2_run_') as work:
        work = Path(work)
        # PCSX2 keeps its data in a PCSX2 folder inside -datapath
        inis = work / 'PCSX2' / 'inis'
        inis.mkdir(parents=True)
        (inis / 'PCSX2.ini').write_text(logging_config(args.config.read_text()))
        log = work / 'pcsx2.log'
        command = [str(args.pcsx2), '-datapath', str(work), '-nogui', '-fastboot', '-logfile', str(log),
                   '-elf', str(elf)]
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        status = None
        loaded = False
        seen = 0
        deadline = time.monotonic() + args.timeout
        try:
            while status is None:
                time.sleep(0.2)
                lines = log.read_text(errors='replace').splitlines() if log.exists() else []
                for line in lines[seen:]:
                    if not loaded:
                        loaded = bool(LOADED.match(line))
                        continue
                    text = TIMESTAMP.sub('', line)
                    if text.strip():
                        print(text, flush=True)
                    if args.until and args.until in text:
                        status = 0
                seen = len(lines)
                if status is None and process.poll() is not None:
                    print(f'pcsx2_run: PCSX2 stopped (status {process.returncode})', file=sys.stderr)
                    status = 2
                elif status is None and time.monotonic() > deadline:
                    if args.until:
                        print(f'pcsx2_run: "{args.until}" did not appear in {args.timeout:g} s', file=sys.stderr)
                    status = 1 if args.until else 0
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill()
            if args.log and log.exists():
                shutil.copyfile(log, args.log)
    if not loaded and status != 0:
        print('pcsx2_run: PCSX2 did not load the program (is a BIOS set in its configuration?)', file=sys.stderr)
    return status


if __name__ == '__main__':
    sys.exit(main())
