#!/usr/bin/env python3
"""Host-side validator for STSYS001 packages. Never loads or executes payloads."""
import struct
import sys
import zlib
from pathlib import Path

HEADER = struct.Struct("<8sHHHHIIQII24s32s8s")
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
ELF_SECTION = struct.Struct("<IIQQQQIIQQ")
ELF_SYMBOL = struct.Struct("<IBBHQQ")

MAX_PACKAGE_SIZE = 16 * 1024 * 1024
SYS_MAGIC = b"STSYS001"
SYS_VERSION = (1, 0)
ARCH_X86_64 = 0x3E
PAYLOAD_ELF64_RELOCATABLE = 1
FLAG_REQUIRES_RELOCATION = 1
ET_REL = 1
EM_X86_64 = 62
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHT_DYNSYM = 11
STB_GLOBAL = 1
STB_WEAK = 2
STT_NOTYPE = 0
STT_FUNC = 2
SHN_UNDEF = 0


class InvalidPackage(Exception):
    pass


def require(condition, message):
    if not condition:
        raise InvalidPackage(message)


def bounded_string(raw, field):
    end = raw.find(b"\0")
    require(end > 0, f"{field} is empty or not NUL-terminated")
    require(not any(raw[end:]), f"{field} has nonzero bytes after terminator")
    value = raw[:end]
    require(all(0x21 <= c <= 0x7E for c in value),
            f"{field} must contain printable ASCII")
    return value


def validate_elf(payload, entry_symbol):
    require(len(payload) >= ELF_HEADER.size, "ELF payload is truncated")

    fields = ELF_HEADER.unpack_from(payload)
    ident, e_type, e_machine, e_version = fields[:4]
    e_shoff = fields[6]
    e_ehsize = fields[8]
    e_shentsize = fields[11]
    e_shnum = fields[12]
    e_shstrndx = fields[13]

    require(ident[:4] == b"\x7fELF", "ELF magic mismatch")
    require(ident[4] == 2, "ELF payload is not ELF64")
    require(ident[5] == 1, "ELF payload is not little-endian")
    require(ident[6] == 1 and e_version == 1, "unsupported ELF version")
    require(e_type == ET_REL, "ELF payload is not relocatable (ET_REL)")
    require(e_machine == EM_X86_64, "ELF machine is not x86-64")
    require(e_ehsize == ELF_HEADER.size, "ELF header size mismatch")
    require(e_shnum > 0, "ELF has no section-header table")
    require(e_shentsize == ELF_SECTION.size, "ELF section-header size mismatch")
    require(e_shoff <= len(payload), "section table offset is out of bounds")
    require(e_shnum <= (len(payload) - e_shoff) // e_shentsize,
            "section table extends beyond payload")
    require(e_shstrndx == 0 or e_shstrndx < e_shnum,
            "section-name table index is invalid")

    sections = []
    for index in range(e_shnum):
        off = e_shoff + index * e_shentsize
        sh = ELF_SECTION.unpack_from(payload, off)
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, \
            sh_link, sh_info, sh_addralign, sh_entsize = sh
        del sh_name, sh_flags, sh_addr, sh_addralign

        require(sh_offset <= len(payload),
                f"section {index} offset is out of bounds")
        if sh_type != SHT_NOBITS:
            require(sh_size <= len(payload) - sh_offset,
                    f"section {index} data extends beyond payload")

        if sh_type in (SHT_SYMTAB, SHT_DYNSYM):
            require(sh_entsize == ELF_SYMBOL.size and
                    sh_size % ELF_SYMBOL.size == 0,
                    f"symbol table {index} has invalid entry sizing")
            require(sh_link < e_shnum, f"symbol table {index} link is invalid")
            require(sections_type(payload, e_shoff, e_shentsize, sh_link)
                    == SHT_STRTAB,
                    f"symbol table {index} does not link to a string table")

        if sh_type in (SHT_REL, SHT_RELA):
            expected = 16 if sh_type == SHT_REL else 24
            require(sh_entsize == expected and sh_size % expected == 0,
                    f"relocation section {index} has invalid entry sizing")
            require(sh_link < e_shnum and sh_info < e_shnum,
                    f"relocation section {index} has invalid links")

        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    found_entry = False
    for index, section in enumerate(sections):
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = section
        if sh_type not in (SHT_SYMTAB, SHT_DYNSYM):
            continue

        _, str_offset, str_size, _, _ = sections[sh_link]
        strings = payload[str_offset:str_offset + str_size]
        symbols = payload[sh_offset:sh_offset + sh_size]

        for pos in range(0, len(symbols), sh_entsize):
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                ELF_SYMBOL.unpack_from(symbols, pos)
            del st_other, st_value, st_size

            require(st_name < len(strings),
                    f"symbol table {index} has invalid name offset")
            end = strings.find(b"\0", st_name)
            require(end >= 0, f"symbol table {index} has unterminated name")
            name = strings[st_name:end]

            if name != entry_symbol:
                continue

            binding = st_info >> 4
            symbol_type = st_info & 0x0F
            if (st_shndx != SHN_UNDEF and binding in (STB_GLOBAL, STB_WEAK)
                    and symbol_type in (STT_NOTYPE, STT_FUNC)):
                found_entry = True

    require(found_entry, "entry symbol not found as a defined global/weak ELF symbol")


def sections_type(payload, shoff, shentsize, index):
    off = shoff + index * shentsize
    require(off + shentsize <= len(payload), "linked section header is out of bounds")
    return ELF_SECTION.unpack_from(payload, off)[1]


def validate(path):
    data = Path(path).read_bytes()
    require(HEADER.size == 104, "internal SYS header layout error")
    require(HEADER.size <= len(data) <= MAX_PACKAGE_SIZE,
            "package size is outside allowed bounds")

    (magic, header_size, version_major, version_minor, reserved0,
     architecture, payload_format, payload_size, payload_crc32, flags,
     module_raw, entry_raw, reserved) = HEADER.unpack_from(data)

    require(magic == SYS_MAGIC, "SYS magic mismatch")
    require(header_size == HEADER.size, "SYS header size mismatch")
    require((version_major, version_minor) == SYS_VERSION,
            "unsupported SYS version")
    require(reserved0 == 0 and not any(reserved),
            "reserved header fields must be zero")
    require(architecture == ARCH_X86_64, "unsupported SYS architecture")
    require(payload_format == PAYLOAD_ELF64_RELOCATABLE,
            "unsupported SYS payload format")
    require(flags == FLAG_REQUIRES_RELOCATION,
            "unsupported SYS flags")
    module_name = bounded_string(module_raw, "module_name")
    entry_symbol = bounded_string(entry_raw, "entry_symbol")

    payload = data[header_size:]
    require(payload_size == len(payload), "payload size does not match file size")
    require((zlib.crc32(payload) & 0xFFFFFFFF) == payload_crc32,
            "payload CRC32 mismatch")

    validate_elf(payload, entry_symbol)
    print(f"PASS: valid STSYS001 package; module={module_name.decode('ascii')}, "
          f"entry={entry_symbol.decode('ascii')}, payload={len(payload)} bytes")
    print("NOTE: validation only; payload was NOT loaded or executed.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <driver.sys>", file=sys.stderr)
        raise SystemExit(2)
    try:
        validate(sys.argv[1])
    except (InvalidPackage, OSError, struct.error, OverflowError) as exc:
        print(f"REJECT: {exc}", file=sys.stderr)
        raise SystemExit(1)
