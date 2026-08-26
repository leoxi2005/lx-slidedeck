#!/usr/bin/env python3
"""Generates a folder of test PNGs for checking the plugin without any PowerPoint.

Each image is a numbered card with a border and corner markers, so letterboxing
(Fit), cropping (Fill) and distortion (Stretch) are all obvious at a glance.

  python3 tools/make-test-deck.py ~/Desktop/LXSD-Test 8 1920 1080
"""
import pathlib, struct, sys, zlib

DIGITS = {
    '0': ["111", "101", "101", "101", "111"], '1': ["010", "110", "010", "010", "111"],
    '2': ["111", "001", "111", "100", "111"], '3': ["111", "001", "111", "001", "111"],
    '4': ["101", "101", "111", "001", "001"], '5': ["111", "100", "111", "001", "111"],
    '6': ["111", "100", "111", "101", "111"], '7': ["111", "001", "010", "010", "010"],
    '8': ["111", "101", "111", "101", "111"], '9': ["111", "101", "111", "001", "111"],
}

PALETTE = [(26, 34, 56), (56, 26, 40), (26, 56, 44), (56, 48, 26),
           (40, 26, 56), (26, 50, 56), (56, 30, 30), (34, 56, 26)]


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def make_card(width, height, index, total):
    bg = PALETTE[(index - 1) % len(PALETTE)]
    fg = (235, 235, 240)
    rows = [bytearray(bg * width) for _ in range(height)]

    def rect(x0, y0, x1, y1, colour):
        x0, y0 = max(0, x0), max(0, y0)
        x1, y1 = min(width, x1), min(height, y1)
        for y in range(y0, y1):
            row = rows[y]
            for x in range(x0, x1):
                row[x * 3:x * 3 + 3] = bytes(colour)

    border = max(4, height // 90)
    rect(0, 0, width, border, fg)
    rect(0, height - border, width, height, fg)
    rect(0, 0, border, height, fg)
    rect(width - border, 0, width, height, fg)

    # corner markers: whichever ones you can still see tells you the scale mode
    m = height // 12
    for (cx, cy) in ((0, 0), (width - m, 0), (0, height - m), (width - m, height - m)):
        rect(cx, cy, cx + m, cy + m, fg)

    # centred step number
    text = str(index)
    cell = height // 12
    gap = cell
    glyph_w = 3 * cell
    total_w = len(text) * glyph_w + (len(text) - 1) * gap
    x = (width - total_w) // 2
    y = (height - 5 * cell) // 2
    for ch in text:
        pattern = DIGITS.get(ch)
        if pattern:
            for gy, line in enumerate(pattern):
                for gx, bit in enumerate(line):
                    if bit == "1":
                        rect(x + gx * cell, y + gy * cell, x + (gx + 1) * cell, y + (gy + 1) * cell, fg)
        x += glyph_w + gap

    # progress ticks along the bottom, one per step
    tick_w = width // (total * 2)
    for i in range(total):
        colour = fg if i < index else tuple(c + 30 for c in bg)
        x0 = (width - total * tick_w * 2) // 2 + i * tick_w * 2
        rect(x0, height - border * 6, x0 + tick_w, height - border * 3, colour)
    return rows


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "LXSD-Test").expanduser()
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 1920
    height = int(sys.argv[4]) if len(sys.argv) > 4 else 1080
    out.mkdir(parents=True, exist_ok=True)
    for i in range(1, count + 1):
        write_png(out / f"step_{i:04d}.png", width, height, make_card(width, height, i, count))
    print(f"{count} cards at {width}x{height} -> {out}")


if __name__ == "__main__":
    main()
