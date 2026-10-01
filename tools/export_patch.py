#!/usr/bin/env python3
"""Export canonical sources plus the gem5 integration diff from a gem5 checkout.

This intentionally writes only generated patch files and mirrored artifact files.
Use a disposable gem5 checkout at the pinned commit, with the current complete
patch already applied. Hand edits belong in this repository's canonical files.
"""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = "c8222cc67a399bfc01e8658dd14b30d5bfd634f9"
ISA = [
    "src/arch/x86/decoder_tables.cc",
    "src/arch/x86/isa/decoder/three_byte_0f38_opcodes.isa",
    "src/arch/x86/isa/includes.isa",
    "src/arch/x86/isa/insts/general_purpose/arithmetic/add_and_subtract.py",
    "src/arch/x86/isa/microops/ldstop.isa",
]
RUBY = [
    "src/mem/ruby/SConscript",
    "src/mem/ruby/common/DataBlock.hh",
    "src/mem/ruby/network/Network.cc",
    "src/mem/ruby/protocol/Kconfig",
    "src/mem/ruby/protocol/RubySlicc_Exports.sm",
    "src/mem/ruby/protocol/RubySlicc_Types.sm",
    "src/mem/ruby/system/Sequencer.cc",
    "src/mem/ruby/system/Sequencer.hh",
]
# O3 CPU: store-class no-return atomics must not forward their delta to younger
# loads, and the optional relaxed (RAO-INT style) ordering knob. Shipped inside
# the integration patch alongside the Ruby changes.
CPU = [
    "src/cpu/o3/BaseO3CPU.py",
    "src/cpu/o3/lsq_unit.cc",
    "src/cpu/o3/lsq_unit.hh",
    # Minor recognises the store-class atomic by its functor (no store data to copy).
    "src/cpu/minor/lsq.cc",
]


def canonical_files():
    pairs = [(p, "src/mem/ruby/protocol/" + p.name)
             for p in sorted((ROOT / "protocol").glob("MOESI_D*"))]
    pairs.append((ROOT / "protocol/DStateEngine.hh", "src/mem/ruby/structures/DStateEngine.hh"))
    pairs.append((ROOT / "protocol/DStateMergeSet.hh", "src/mem/ruby/structures/DStateMergeSet.hh"))
    pairs += [(ROOT / "configs/MOESI_D.py", "configs/ruby/MOESI_D.py"),
              (ROOT / "configs/rrt_big.py", "configs/example/rrt_big.py")]
    for folder, patterns in [("bench", ["*.c", "*.h", "*.mtx", "Makefile"]),
                             ("build_opts", ["X86_MOESI_*"])]:
        # as_posix(): git diff headers are always forward-slashed, so a Windows
        # os.sep here emits `a/bench\cms_bench.c` and breaks the patch-mirror check.
        pairs += [(p, p.relative_to(ROOT).as_posix()) for pattern in patterns
                  for p in sorted((ROOT / folder).glob(pattern))]
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gem5", type=Path)
    args = parser.parse_args()
    gem5 = args.gem5.resolve()
    def git(*argv):
        return subprocess.check_output(["git", "-C", str(gem5), *argv])
    if git("rev-parse", "HEAD").decode().strip() != BASE:
        parser.error("gem5 HEAD must be v25.1.0.1 at the pinned commit")
    pairs = canonical_files()
    for source, relative in pairs:
        dest = gem5 / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
    paths = sorted({relative for _, relative in pairs})
    # Intent-to-add is limited to the mirrored artifact, never unrelated files.
    git("add", "-N", "--", *paths)
    outputs = {
        "isa/stula-x86-isa.patch": ISA,
        "protocol/ruby-integration.patch": RUBY + CPU + ["src/mem/ruby/structures/DStateEngine.hh",
                                                         "src/mem/ruby/structures/DStateMergeSet.hh"],
        "gem5-moesi-d.patch": ISA + RUBY + CPU + paths,
    }
    for filename, selected in outputs.items():
        data = git("diff", "--binary", "--no-ext-diff", BASE, "--", *sorted(set(selected)))
        (ROOT / filename).write_bytes(data)
        print(f"exported {filename}: {len(data)} bytes")


if __name__ == "__main__":
    main()
