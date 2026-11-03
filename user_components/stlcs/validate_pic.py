#!/usr/bin/env python3
"""Reject absolute memory operands in the linked Starlix Lock Screen image.

ECT v1 maps component sections at PREFERRED_BASE + section_vaddr.  Code must
therefore address image-local data through RIP-relative references, not fixed
low virtual addresses baked into the instructions.
"""
from __future__ import annotations

import argparse
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

# GNU objdump's Intel syntax prints e.g. `QWORD PTR ds:0x3009`; LLVM's form is
# commonly `qword ptr [0x3009]`. Both indicate a fixed absolute memory address.
ABSOLUTE_MEMORY = re.compile(
    r"(?:\b[a-z]{1,2}:0x[0-9a-f]+\b|"
    r"\[(?:[a-z]{1,2}:)?\s*0x[0-9a-f]+\])",
    re.IGNORECASE,
)


def choose_objdump(explicit: str | None) -> list[str]:
    configured = explicit or os.environ.get("OBJDUMP")
    if configured:
        command = shlex.split(configured)
        if command and shutil.which(command[0]):
            return command
        raise FileNotFoundError(f"objdump command not found: {configured}")
    for candidate in ("llvm-objdump", "objdump"):
        resolved = shutil.which(candidate)
        if resolved:
            return [resolved]
    raise FileNotFoundError("neither llvm-objdump nor objdump is installed")


def validate(elf: pathlib.Path, objdump: str | None) -> None:
    if not elf.is_file() or elf.stat().st_size == 0:
        raise ValueError(f"ELF is missing or empty: {elf}")

    command = choose_objdump(objdump) + ["-d", "-Mintel", str(elf)]
    result = subprocess.run(command, check=False, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(
            f"disassembly failed ({result.returncode}): "
            f"{result.stderr.strip() or result.stdout.strip()}"
        )

    offenders: list[tuple[int, str]] = []
    for line_number, line in enumerate(result.stdout.splitlines(), start=1):
        if ABSOLUTE_MEMORY.search(line):
            offenders.append((line_number, line.strip()))

    if offenders:
        details = "\n".join(
            f"  disassembly line {number}: {line}"
            for number, line in offenders[:12]
        )
        more = "" if len(offenders) <= 12 else f"\n  ... and {len(offenders) - 12} more"
        raise ValueError(
            f"found {len(offenders)} absolute memory operand(s); "
            "ECT image-local references must be RIP-relative:\n"
            f"{details}{more}"
        )

    print(
        f"[stlcs-pic] PASS: no absolute memory operands in {elf} "
        f"(disassembled with {' '.join(command[:-3]) or command[0]})"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("--objdump", help="objdump command (defaults to OBJDUMP, llvm-objdump, or objdump)")
    args = parser.parse_args()
    try:
        validate(args.elf, args.objdump)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"[stlcs-pic] FAIL: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
