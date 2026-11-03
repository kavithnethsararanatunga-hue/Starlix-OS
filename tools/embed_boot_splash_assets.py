#!/usr/bin/env python3
"""Embed Starlix boot PNG/SVG assets as freestanding C arrays (stdlib only).

PNG support is intentionally strict: 8-bit, non-interlaced RGBA PNG. That is
exactly the Stlg.png boot-logo asset contract. The image is bilinear-resized to
512x512 RGBA for a compact, high-quality UEFI binary.
"""
from __future__ import annotations
import argparse
import math
import struct
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
TARGET = 512

def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def decode_rgba_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError(f"{path}: not a PNG file")
    pos = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos+4:pos+8]
        start = pos + 8
        end = start + length
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        chunk = data[start:end]
        if kind == b"IHDR":
            width, height, bit_depth, color_type, comp, filt, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat.extend(chunk)
        elif kind == b"IEND":
            break
        pos = end + 4
    if not width or not height or bit_depth != 8 or color_type != 6 or interlace != 0:
        raise ValueError("Stlg.png must be non-interlaced 8-bit RGBA PNG")
    bpp = 4
    stride = width * bpp
    raw = zlib.decompress(bytes(idat))
    expected = (stride + 1) * height
    if len(raw) != expected:
        raise ValueError(f"PNG decompressed length mismatch: {len(raw)} != {expected}")
    pixels = bytearray(stride * height)
    src = 0
    for y in range(height):
        filt = raw[src]
        src += 1
        row_start = y * stride
        prev_start = (y - 1) * stride
        for x in range(stride):
            value = raw[src]
            src += 1
            left = pixels[row_start + x - bpp] if x >= bpp else 0
            up = pixels[prev_start + x] if y else 0
            upper_left = pixels[prev_start + x - bpp] if y and x >= bpp else 0
            if filt == 1:
                value = (value + left) & 255
            elif filt == 2:
                value = (value + up) & 255
            elif filt == 3:
                value = (value + ((left + up) // 2)) & 255
            elif filt == 4:
                value = (value + paeth(left, up, upper_left)) & 255
            elif filt != 0:
                raise ValueError(f"unsupported PNG filter {filt}")
            pixels[row_start + x] = value
    # Drop only practically invisible alpha noise. This keeps the supplied logo
    # appearance intact while preventing isolated near-transparent edge specks.
    for i in range(0, len(pixels), 4):
        if pixels[i + 3] <= 2:
            pixels[i:i + 4] = b"\x00\x00\x00\x00"
    return width, height, bytes(pixels)

def resize_bilinear_rgba(width: int, height: int, src: bytes, target: int) -> bytes:
    """Bilinear resize in premultiplied-alpha space to avoid colored edge halos."""
    out = bytearray(target * target * 4)
    for y in range(target):
        fy = ((y + 0.5) * height / target) - 0.5
        y0 = max(0, min(height - 1, int(math.floor(fy))))
        y1 = min(height - 1, y0 + 1)
        wy = max(0.0, min(1.0, fy - y0))
        for x in range(target):
            fx = ((x + 0.5) * width / target) - 0.5
            x0 = max(0, min(width - 1, int(math.floor(fx))))
            x1 = min(width - 1, x0 + 1)
            wx = max(0.0, min(1.0, fx - x0))
            i00 = (y0 * width + x0) * 4
            i10 = (y0 * width + x1) * 4
            i01 = (y1 * width + x0) * 4
            i11 = (y1 * width + x1) * 4
            dst = (y * target + x) * 4
            alpha_values = [src[i00+3], src[i10+3], src[i01+3], src[i11+3]]
            a_top = alpha_values[0] * (1-wx) + alpha_values[1] * wx
            a_bottom = alpha_values[2] * (1-wx) + alpha_values[3] * wx
            alpha = a_top * (1-wy) + a_bottom * wy
            out[dst+3] = max(0, min(255, int(round(alpha))))
            if alpha <= 0.0001:
                out[dst:dst+3] = b"\x00\x00\x00"
                continue
            for c in range(3):
                p00 = src[i00+c] * alpha_values[0] / 255.0
                p10 = src[i10+c] * alpha_values[1] / 255.0
                p01 = src[i01+c] * alpha_values[2] / 255.0
                p11 = src[i11+c] * alpha_values[3] / 255.0
                top_value = p00 * (1-wx) + p10 * wx
                bottom_value = p01 * (1-wx) + p11 * wx
                premultiplied = top_value * (1-wy) + bottom_value * wy
                straight = premultiplied * 255.0 / alpha
                out[dst+c] = max(0, min(255, int(round(straight))))
    return bytes(out)

def emit_array(f, name: str, data: bytes) -> None:
    f.write(f"static const uint8_t {name}[] = {{\n")
    for i in range(0, len(data), 24):
        f.write("    " + ",".join(f"0x{x:02X}" for x in data[i:i+24]) + ",\n")
    f.write("};\n")

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("png", type=Path)
    ap.add_argument("svg", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    width, height, rgba = decode_rgba_png(args.png)
    embedded = resize_bilinear_rgba(width, height, rgba, TARGET)
    svg = args.svg.read_text(encoding="utf-8")
    # The supplied SVG had a misspelled namespace URI. Correct the embedded copy.
    svg = svg.replace('xmlns="http://w3.org"', 'xmlns="http://www.w3.org/2000/svg"')
    if ")STLIXSVG\"" in svg:
        raise ValueError("SVG contains the reserved raw-string terminator")
    if "<svg" not in svg or "<circle" not in svg or "<linearGradient" not in svg:
        raise ValueError("spinner SVG is outside the supported svg/gradient/circle subset")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by tools/embed_boot_splash_assets.py. Do not hand-edit.\n")
        f.write('#pragma once\n#include "efi/efi.hpp"\n')
        f.write(f"static constexpr uint32_t kStlgLogoWidth = {TARGET}u;\n")
        f.write(f"static constexpr uint32_t kStlgLogoHeight = {TARGET}u;\n")
        emit_array(f, "kStlgLogoRGBA", embedded)
        f.write("static const char kStarlixBootSpinnerSvg[] = R\"STLIXSVG(\n")
        f.write(svg)
        if not svg.endswith("\n"):
            f.write("\n")
        f.write(")STLIXSVG\";\n")
    print(f"[boot-assets] PNG {width}x{height} RGBA -> embedded {TARGET}x{TARGET} RGBA ({len(embedded)} bytes)")
    print(f"[boot-assets] SVG embedded ({len(svg.encode('utf-8'))} bytes; namespace normalized)")
    print(f"[boot-assets] generated {args.output}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
