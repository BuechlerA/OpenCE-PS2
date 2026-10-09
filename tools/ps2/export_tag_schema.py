#!/usr/bin/env python3
"""Exports the tag schema (port/linux/game/tag_schema_*.c) as JSON, for the
PS2 map tools (map_budget.py, converter_prototype.py):

    python tools/ps2/export_tag_schema.py [--libc-include DIR] [--output FILE]

The schema is a set of static tables whose offsets and sizes the compiler
works out from the game's own structures (tag_schema.h). This script
compiles the schema files for the Xbox's ABI with the Linux build's flags
(tools/linux_build.py) to LLVM IR, and reads the tables' initializers from
the IR. The output (tools/ps2/tag_schema.json by default, committed) lists
each definition (name, size, fields) and each tag group (parents,
definition).

Run it from the repository root, with clang. On Linux the system's 32-bit
C headers are used. Elsewhere, give --libc-include a 32-bit x86 C library's
headers, for example musl's: `make ARCH=i386 prefix=DIR install-headers`
in a musl source tree, then --libc-include DIR/include.
"""

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.linux_build import (  # noqa: E402
    GAME_FLAGS, LINUX_ABI_FLAGS, PORT_DIR, XDK_INCLUDE, _load_port_config)

SCHEMA_FILES = sorted(Path("port/linux/game").glob("tag_schema_*.c"))
GROUP_LISTS = ("tag_schema_object_groups", "tag_schema_model_groups", "tag_schema_collision_groups",
               "tag_schema_render_groups", "tag_schema_effect_groups", "tag_schema_scenario_groups")
GLOBAL = re.compile(r'^@([\w.]+) = [^=]*?(?:constant|global) (.*?)(?:, align \d+)?$')

# tag_schema.h, enum tag_schema_field_type
BLOCK, REFERENCE, TAG_INDEX, STRUCT, TERMINATOR = 0, 3, 4, 9, 11


class Parser:
    """an LLVM IR constant: arrays and structures become lists, strings
    str, integers int, null None, zeroinitializer 0, @x ('ref', name)"""

    def __init__(self, text, prefix):
        self.t = text
        self.i = 0
        self.prefix = prefix

    def ws(self):
        while self.i < len(self.t) and self.t[self.i] == ' ':
            self.i += 1

    def peek(self, s):
        self.ws()
        return self.t.startswith(s, self.i)

    def eat(self, s):
        self.ws()
        if not self.t.startswith(s, self.i):
            raise ValueError(f'expected {s!r} at {self.t[self.i:self.i + 60]!r}')
        self.i += len(s)

    def match(self, pattern):
        m = re.compile(pattern).match(self.t, self.i)
        if not m:
            raise ValueError(f'unexpected {self.t[self.i:self.i + 60]!r}')
        self.i = m.end()
        return m

    def aggregate(self, element):
        if self.peek('['):
            close = ']'
        elif self.peek('<{'):
            close = '}>'
        elif self.peek('{'):
            close = '}'
        else:
            return None
        self.eat({']': '[', '}>': '<{', '}': '{'}[close])
        out = []
        while not self.peek(close[0]):
            out.append(element())
            if self.peek(','):
                self.eat(',')
        self.eat(close)
        return out

    def type_(self):
        self.ws()
        if self.peek('['):
            self.eat('[')
            self.match(r'\d+')
            self.eat('x')
            self.type_()
            self.eat(']')
        elif self.aggregate(self.type_) is None:
            self.match(r'[%\w.]+')

    def typed_value(self):
        self.type_()
        return self.value()

    def value(self):
        self.ws()
        if self.peek('c"'):
            self.eat('c"')
            j = self.t.index('"', self.i)
            raw = self.t[self.i:j]
            self.i = j + 1
            return re.sub(r'\\([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), raw).rstrip('\0')
        out = self.aggregate(self.typed_value)
        if out is not None:
            return out
        for word, result in (('null', None), ('zeroinitializer', 0)):
            if self.peek(word):
                self.eat(word)
                return result
        if self.peek('@'):
            return ('ref', self.prefix + self.match(r'@([\w.]+)').group(1))
        return int(self.match(r'-?\d+').group())


def compile_ir(clang, libc_include, work):
    """the schema files' IR, compiled as the Linux build compiles the game"""
    config = _load_port_config()
    semantics = work / "halo_msvc_semantics.h"
    subprocess.run([sys.executable, "tools/linux_msvc_semantics.py", "--output", str(semantics),
                    "--all-inlines", "--tags", "source", "--inlines", "source", "--inlines", str(XDK_INCLUDE)],
                   check=True)
    flags = [*LINUX_ABI_FLAGS, *GAME_FLAGS, "-O0",
             "-include", str(PORT_DIR / "include" / "halo_linux_prefix.h"), "-include", str(semantics),
             f"-I{PORT_DIR / 'include'}", "-iquote", config["game_sources"],
             *[f"-D{d}" for d in config["game"].get("defines", [])],
             *[f"-I{d}" for d in config["game"]["include_dirs"]],
             "-idirafter", str(XDK_INCLUDE)]
    if libc_include:
        flags += ["-nostdinc", "-idirafter", libc_include, "-isystem",
                  subprocess.run([clang, "-print-resource-dir"], check=True, capture_output=True,
                                 text=True).stdout.strip() + "/include"]
    outputs = []
    for source in SCHEMA_FILES:
        out = work / (source.stem + ".ll")
        subprocess.run([clang, *flags, "-S", "-emit-llvm", str(source), "-o", str(out)], check=True)
        outputs.append(out)
    return outputs


def load(paths):
    values = {}
    for path in paths:
        prefix = path.stem + ':'
        for line in path.read_text().splitlines():
            m = GLOBAL.match(line)
            if not m or ' external ' in line:
                continue
            p = Parser(m.group(2), prefix)
            p.type_()
            values[prefix + m.group(1)] = p.value()
    return values


def export(values):
    # an extern global is referenced by its bare name from other files
    by_bare = {}
    for key in values:
        by_bare.setdefault(key.split(':', 1)[1], []).append(key)

    def resolve(ref):
        return ref[1] if ref[1] in values else by_bare[ref[1].split(':', 1)[1]][0]

    definitions = {}

    def definition(ref):
        key = resolve(ref)
        if key in definitions:
            return key
        name_ref, size, fields_ref = values[key]
        definitions[key] = None
        fields = []
        for f in values[resolve(fields_ref)]:
            if f == 0 or f[0] == TERMINATOR:
                break
            ftype, fsize, count, flags, offset, maximum, _level, _target, dref, _check, nref = f
            entry = dict(type=ftype, size=fsize, count=count, flags=flags, offset=offset, maximum=maximum,
                         name=values[resolve(nref)] if nref else None)
            if ftype in (BLOCK, STRUCT) and dref:
                entry['definition'] = definition(dref)
            elif ftype in (REFERENCE, TAG_INDEX) and dref:
                entry['groups'] = [g for g in values[resolve(dref)] if g]
            fields.append(entry)
        definitions[key] = dict(name=values[resolve(name_ref)], size=size, fields=fields)
        return key

    def tag(value):
        return (value & 0xFFFFFFFF).to_bytes(4, 'big').decode('latin-1')

    groups = {}
    for list_name in GROUP_LISTS:
        key = next(k for k in by_bare[list_name] if values[k] != 0)
        for element in values[key]:
            if not element or not element[0]:
                break
            group_tag, parents, dref = element
            groups[tag(group_tag)] = dict(
                parents=[tag(p) for p in (parents or []) if p and p != -1],
                definition=definition(dref) if dref else None)
    return dict(groups=groups, definitions=definitions)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--libc-include', help="a 32-bit x86 C library's headers (not needed on Linux)")
    parser.add_argument('--output', type=Path, default=Path('tools/ps2/tag_schema.json'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        schema = export(load(compile_ir(args.clang, args.libc_include, Path(work))))
    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(schema, f, indent=1, sort_keys=True)
        f.write('\n')
    print(f"{args.output}: {len(schema['groups'])} groups, {len(schema['definitions'])} definitions")


if __name__ == '__main__':
    main()
