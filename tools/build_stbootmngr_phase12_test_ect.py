#!/usr/bin/env python3
"""Create a tiny Starlix ECT test payload that emits a UART marker then halts."""
from pathlib import Path
import struct
import sys
import zlib

if len(sys.argv) != 2:
    raise SystemExit(f"Usage: {sys.argv[0]} <output.ect>")

message = b"\r\n[PHASE12 TEST KERNEL] ENTRY AFTER ExitBootServices\r\n"
code = bytearray()
for ch in message:
    # Poll COM1 line-status register until transmitter holding register is empty.
    code += b"\x66\xba\xfd\x03"       # mov dx, 0x3fd
    code += b"\xec\xa8\x20\x74\xfb"   # in al,dx; test al,0x20; jz back to in
    code += b"\x66\xba\xf8\x03"       # mov dx, 0x3f8
    code += b"\xb0" + bytes([ch]) + b"\xee"  # mov al,ch; out dx,al
code += b"\xfa\xf4"                   # cli; hlt forever

header = bytearray(128)
header[0:2] = b"Px"
header[2:34] = b"Starlix ect @PX".ljust(32, b"\0")
header[34:74] = b"Starlix Native Executable Pack".ljust(40, b"\0")
struct.pack_into("<HHBBH", header, 74, 1, 0, 1, 1, 128)
struct.pack_into("<IIIIII", header, 82, 1, 128, 0, 152, 0, 152)
struct.pack_into("<QQI", header, 106, 0x200000, 0x200000, zlib.crc32(code) & 0xffffffff)

section = bytearray(24)
section[0:8] = b".text\0\0\0"
struct.pack_into("<IIII", section, 8, 0x200000, len(code), 152, 0x5)

out = Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(header + section + code)

# Validate each field consumed by Rxload's current ECT loader.
raw = out.read_bytes()
assert raw[:2] == b"Px"
assert struct.unpack_from("<H", raw, 80)[0] == 128
assert struct.unpack_from("<I", raw, 82)[0] == 1
assert struct.unpack_from("<I", raw, 90)[0] == 0
assert struct.unpack_from("<I", raw, 98)[0] == 0
assert struct.unpack_from("<Q", raw, 106)[0] == 0x200000
assert struct.unpack_from("<I", raw, 122)[0] == (zlib.crc32(code) & 0xffffffff)
assert struct.unpack_from("<I", raw, 140)[0] == len(code)
assert struct.unpack_from("<I", raw, 144)[0] == 152
assert len(raw) == 152 + len(code)
print(f"PASS: test ECT created ({len(raw)} bytes; payload {len(code)} bytes; entry 0x200000)")
