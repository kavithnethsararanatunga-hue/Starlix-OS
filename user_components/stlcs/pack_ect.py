#!/usr/bin/env python3
"""Package a linked x86-64 ELF into the documented Starlix ECT v1 container."""
from __future__ import annotations
import argparse
import binascii
import pathlib
import struct
import sys

MAGIC = b"Px"
BANNER = b"Starlix ect @PX"
FORMAT = b"Starlix Native Executable Pack"
HEADER = struct.Struct("<2s32s40sHHBBHIIIIIIQQI2s")
SECTION = struct.Struct("<8sIIII")
PREFERRED_BASE = 0x0000008000100000
SHF_ALLOC = 0x2
SHF_WRITE = 0x1
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8
SHT_RELA = 4
SHT_REL = 9


def parse_elf(path: pathlib.Path):
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError("input is not an ELF file")
    if data[4] != 2 or data[5] != 1 or data[6] != 1:
        raise ValueError("expected ELF64 little-endian version 1")
    eh = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
    e_type, machine, version, entry = eh[1], eh[2], eh[3], eh[4]
    shoff, shentsize, shnum, shstrndx = eh[6], eh[11], eh[12], eh[13]
    if machine != 62 or e_type not in (2, 3) or version != 1:
        raise ValueError("expected linked x86-64 ET_EXEC/ET_DYN ELF")
    if shentsize != 64 or shnum == 0 or shstrndx >= shnum:
        raise ValueError("invalid ELF section table")
    if shoff > len(data) or shnum > (len(data) - shoff) // shentsize:
        raise ValueError("ELF section table is out of bounds")
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
               for i in range(shnum)]
    strhdr = headers[shstrndx]
    str_off, str_size = strhdr[4], strhdr[5]
    if str_off > len(data) or str_size > len(data) - str_off:
        raise ValueError("ELF section-name table is out of bounds")
    names = data[str_off:str_off + str_size]

    def name_of(offset: int) -> str:
        if offset >= len(names):
            return ""
        end = names.find(b"\0", offset)
        if end < 0:
            return ""
        return names[offset:end].decode("ascii", "replace")

    sections = []
    for index, sh in enumerate(headers):
        name_off, stype, flags, addr, offset, size, link, info, align, entsize = sh
        name = name_of(name_off)
        if stype in (SHT_REL, SHT_RELA) and size:
            raise ValueError(f"unresolved relocation section remains: {name}")
        if not (flags & SHF_ALLOC) or size == 0:
            continue
        if stype == SHT_NOBITS:
            raise ValueError(f"ECT v1 has no memory-size field; non-empty {name} is not packageable")
        if offset > len(data) or size > len(data) - offset:
            raise ValueError(f"ELF section is out of bounds: {name}")
        if addr > 0xFFFFFFFF or size > 0xFFFFFFFF:
            raise ValueError(f"ECT v1 section field overflow: {name}")
        if name not in (".text", ".rodata", ".data"):
            raise ValueError(f"unsupported allocatable ELF section: {name}")
        sec_flags = 0x1  # SEC_READ
        if flags & SHF_WRITE:
            sec_flags |= 0x2
        if flags & SHF_EXECINSTR:
            sec_flags |= 0x4
        sections.append({"name": name, "addr": addr, "flags": sec_flags,
                         "bytes": data[offset:offset + size], "index": index})

    sections.sort(key=lambda item: item["addr"])
    if not sections or not any(s["name"] == ".text" for s in sections):
        raise ValueError("ELF has no executable .text section")
    for i, sec in enumerate(sections):
        if i and sec["addr"] < sections[i - 1]["addr"] + len(sections[i - 1]["bytes"]):
            raise ValueError("overlapping ECT virtual sections")
    text = next(s for s in sections if s["name"] == ".text")
    if not (text["addr"] <= entry < text["addr"] + len(text["bytes"])):
        raise ValueError("ELF entry point is not inside .text")
    return entry, sections


def package(input_path: pathlib.Path, output_path: pathlib.Path) -> None:
    entry, sections = parse_elf(input_path)
    count = len(sections)
    section_table_off = HEADER.size
    import_table_off = section_table_off + count * SECTION.size
    reloc_table_off = import_table_off  # zero imports; zero relocations
    data_offset = reloc_table_off
    section_headers = []
    payload = bytearray()
    for section in sections:
        raw = section["bytes"]
        raw_offset = data_offset + len(payload)
        section_headers.append(SECTION.pack(
            section["name"].encode("ascii")[:8].ljust(8, b"\0"),
            section["addr"], len(raw), raw_offset, section["flags"]))
        payload.extend(raw)

    text = next(s["bytes"] for s in sections if s["name"] == ".text")
    checksum = binascii.crc32(text) & 0xFFFFFFFF
    header = HEADER.pack(
        MAGIC, BANNER.ljust(32, b"\0"), FORMAT.ljust(40, b"\0"),
        1, 0, 1, 1, HEADER.size, count, section_table_off,
        0, import_table_off, 0, reloc_table_off, entry,
        PREFERRED_BASE, checksum, b"\0\0")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(header + b"".join(section_headers) + payload)
    print(f"[stlcs-ect] ECT v1 packaged: {output_path}")
    print(f"[stlcs-ect] entry=0x{entry:X}, preferred_base=0x{PREFERRED_BASE:X}, sections={count}, bytes={output_path.stat().st_size}")
    for section in sections:
        print(f"[stlcs-ect] {section['name']}: vaddr=0x{section['addr']:X}, bytes={len(section['bytes'])}, flags=0x{section['flags']:X}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_elf", type=pathlib.Path)
    parser.add_argument("output_ect", type=pathlib.Path)
    args = parser.parse_args()
    try:
        package(args.input_elf, args.output_ect)
    except (OSError, ValueError, struct.error) as exc:
        print(f"[stlcs-ect] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
