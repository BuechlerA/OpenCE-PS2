#!/usr/bin/env python3

# root build script: writes build.ninja for the PS2 port (`ninja ps2`)

import argparse
import io
import os
import sys
from pathlib import Path
from types import SimpleNamespace

from tools import ninja_syntax
from tools.ps2_build import generate_ps2_build, ps2_configure_inputs

# arguments
parser = argparse.ArgumentParser()
parser.add_argument(
    "--release",
    action="store_true",
    help="release build: assertions are not checked, as in the retail game",
)
parser.add_argument(
    "--ps2dev",
    type=str,
    help="the ps2dev toolchain for `ninja ps2` (default: PS2DEV; https://github.com/ps2dev/ps2dev)",
)
args = parser.parse_args()

# the settings the build reads
sln = SimpleNamespace(
    build_dir=Path("build"),
    port_release=args.release,
    ps2dev=args.ps2dev,
)

# build.ninja
out = io.StringIO()
n = ninja_syntax.Writer(out)
n.variable("ninja_required_version", "1.3")
n.newline()

configure_script = Path(os.path.relpath(os.path.abspath(sys.argv[0])))
n.comment("The arguments passed to configure.py, for rerunning it.")
n.variable(
    "configure_args",
    [f'"{arg}"' if any(ch.isspace() for ch in arg) else arg for arg in sys.argv[1:]],
)
n.variable("python", f'"{sys.executable}"')
n.newline()

generate_ps2_build(n, sln)

n.comment("Reconfigure on change")
n.rule(
    name="configure",
    command=f"$python {configure_script} $configure_args",
    generator=True,
    description=f"RUN {configure_script}",
)
n.build(
    outputs="build.ninja",
    rule="configure",
    implicit=[
        configure_script,
        Path("tools/ninja_syntax.py"),
        *ps2_configure_inputs(),
    ],
)
n.newline()

# `ninja` alone builds the PS2 game where its toolchain was found
if "\nbuild ps2: " in out.getvalue():
    n.comment("Default rule")
    n.default("ps2")

with open("build.ninja", "w", encoding="utf-8") as f:
    f.write(out.getvalue())
out.close()
