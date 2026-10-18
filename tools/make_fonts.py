"""Tạo font khử răng cưa (định dạng VLW, dùng được cả với TFT_eSPI/LovyanGFX) từ file TTF.

Cách dùng (cần: pip install freetype-py):
    python tools/make_fonts.py tools/fonts include/fonts

Mỗi font sinh ra một file include/fonts/<tên>.h chứa mảng byte của file VLW.
"""
import struct
import sys
from pathlib import Path

import freetype

# Bộ ký tự: ASCII, Latin-1, toàn bộ chữ tiếng Việt và một số dấu câu hay gặp trong tin nhắn
TEXT = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x100))
    + [0x102, 0x103, 0x110, 0x111, 0x128, 0x129, 0x168, 0x169, 0x1A0, 0x1A1, 0x1AF, 0x1B0]
    + list(range(0x1EA0, 0x1EFA))
    + [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2039, 0x203A, 0x20AB, 0x20AC]
)
DIGITS = [ord(c) for c in "0123456789"]

# (tên mảng, file TTF, cỡ chữ theo pixel, bộ ký tự)
FONTS = [
    ("font_small", "BeVietnamPro-Medium.ttf", 14, TEXT),
    ("font_label", "BeVietnamPro-SemiBold.ttf", 14, TEXT),
    ("font_body", "BeVietnamPro-Regular.ttf", 17, TEXT),
    ("font_title", "BeVietnamPro-Bold.ttf", 21, TEXT),
    ("font_digits", "BeVietnamPro-Bold.ttf", 44, DIGITS),
]

# Chữ sáng trên nền tối trông mảnh hơn thực tế khi trộn màu tuyến tính -> tăng nhẹ độ phủ
ALPHA_GAMMA = 0.8


def render_vlw(ttf_path: Path, size_px: int, codepoints):
    face = freetype.Face(str(ttf_path))
    face.set_pixel_sizes(0, size_px)
    records, bitmaps, missing = [], [], []
    for cp in sorted(set(codepoints)):
        if face.get_char_index(cp) == 0:
            missing.append(cp)
            continue
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
        g = face.glyph
        bm = g.bitmap
        w, h, pitch = bm.width, bm.rows, bm.pitch
        data = bytearray()
        for row in range(h):
            for a in bm.buffer[row * pitch: row * pitch + w]:
                data.append(round(255 * (a / 255) ** ALPHA_GAMMA) if a else 0)
        advance = round(g.advance.x / 64)
        # Bản ghi VLW: unicode, cao, rộng, bước tiến x, khoảng từ baseline lên đỉnh, lệch trái, 0
        records.append(struct.pack(">7i", cp, h, w, advance, g.bitmap_top, g.bitmap_left, 0))
        bitmaps.append(bytes(data))
    ascent = round(face.size.ascender / 64)
    descent = round(-face.size.descender / 64)
    header = struct.pack(">6i", len(records), 11, size_px, 0, ascent, descent)
    return header + b"".join(records) + b"".join(bitmaps), len(records), missing


def write_header(path: Path, name: str, blob: bytes, comment: str):
    lines = [
        f"// {comment}",
        "// Tạo tự động bởi tools/make_fonts.py, không sửa tay.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"static const uint8_t {name}[{len(blob)}] = {{",
    ]
    for i in range(0, len(blob), 24):
        lines.append("  " + ",".join(f"0x{b:02X}" for b in blob[i:i + 24]) + ",")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # console Windows mặc định không in được tiếng Việt
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ttf_dir, out_dir = Path(sys.argv[1]), Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, ttf, size, chars in FONTS:
        blob, count, missing = render_vlw(ttf_dir / ttf, size, chars)
        comment = f"{ttf} (SIL OFL 1.1), {size}px, {count} ký tự"
        write_header(out_dir / f"{name}.h", name, blob, comment)
        miss = " ".join(f"U+{cp:04X}" for cp in missing) or "không"
        print(f"{name:12s} {ttf:26s} {size:3d}px  {count:3d} ký tự  {len(blob):7d} byte  thiếu: {miss}")


if __name__ == "__main__":
    main()
