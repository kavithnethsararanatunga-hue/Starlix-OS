#!/usr/bin/env python3
"""Strict structural validator for the Starlix ECT v1 image emitted for stlcs."""
from __future__ import annotations
import binascii
import pathlib
import struct
import sys

HEADER = struct.Struct("<2s32s40sHHBBHIIIIIIQQI2s")
SECTION = struct.Struct("<8sIIII")
PREFERRED_BASE = 0x0000008000100000

def validate(path: pathlib.Path) -> None:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("file is smaller than ECT header")
    fields = HEADER.unpack_from(blob)
    (magic, banner, fmt, major, minor, image_type, arch, header_size,
     section_count, section_off, import_count, import_off, reloc_count,
     reloc_off, entry, preferred_base, checksum, reserved) = fields
    if magic != b"Px" or not banner.startswith(b"Starlix ect @PX"):
        raise ValueError("bad ECT magic/banner")
    if not fmt.startswith(b"Starlix Native Executable Pack"):
        raise ValueError("bad ECT format name")
    if (major, minor, image_type, arch, header_size) != (1, 0, 1, 1, 128):
        raise ValueError("unsupported ECT version/type/architecture/header size")
    if preferred_base != PREFERRED_BASE or reserved != b"\0\0":
        raise ValueError("unexpected preferred base or reserved bits")
    if import_count != 0 or reloc_count != 0:
        raise ValueError("stlcs v1 must not require imports or relocations")
    if section_off != HEADER.size or section_count == 0:
        raise ValueError("invalid section table")
    table_end = section_off + section_count * SECTION.size
    if table_end > len(blob) or import_off != table_end or reloc_off != import_off:
        raise ValueError("invalid table offsets")

    names = set()
    loaded = []
    text_for_crc = None
    for i in range(section_count):
        name_raw, vaddr, size, offset, flags = SECTION.unpack_from(
            blob, section_off + i * SECTION.size)
        name = name_raw.split(b"\0", 1)[0].decode("ascii")
        if name in names:
            raise ValueError(f"duplicate section {name}")
        names.add(name)
        if name not in (".text", ".rodata", ".data"):
            raise ValueError(f"unexpected section {name}")
        if size == 0 or offset < table_end or offset > len(blob) or size > len(blob) - offset:
            raise ValueError(f"invalid raw range for {name}")
        if vaddr + size > 0x100000000:
            raise ValueError(f"virtual range overflow for {name}")
        if flags & ~0x7 or not (flags & 1):
            raise ValueError(f"invalid section flags for {name}")
        if flags & 2 and flags & 4:
            raise ValueError(f"W^X violation in {name}")
        loaded.append((vaddr, vaddr + size, offset, offset + size, name, flags))
        if name == ".text":
            if not (flags & 4) or (flags & 2):
                raise ValueError(".text must be read+execute and not writable")
            text_for_crc = blob[offset:offset + size]
        elif name == ".rodata" and flags != 1:
            raise ValueError(".rodata must be read-only")
        elif name == ".data" and flags != 3:
            raise ValueError(".data must be read+write and non-executable")

    if ".text" not in names or ".rodata" not in names:
        raise ValueError("stlcs requires .text and .rodata")
    if text_for_crc is None or binascii.crc32(text_for_crc) & 0xFFFFFFFF != checksum:
        raise ValueError(".text CRC32 mismatch")
    text_range = next((lo, hi) for lo, hi, _, _, name, _ in loaded if name == ".text")
    if not (text_range[0] <= entry < text_range[1]):
        raise ValueError("entry point is outside .text")
    loaded.sort()
    for a, b in zip(loaded, loaded[1:]):
        if a[1] > b[0] or a[3] > b[2]:
            raise ValueError("overlapping virtual or raw sections")
    if len(blob) != max(item[3] for item in loaded):
        raise ValueError("trailing or unreferenced bytes found")
    print(f"[stlcs-ect] PASS: {path} ({len(blob)} bytes, entry=0x{entry:X}, {section_count} sections)")

if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: validate_ect.py <component.ect>")
        validate(pathlib.Path(sys.argv[1]))
    except (OSError, ValueError, struct.error) as exc:
        print(f"[stlcs-ect] FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
