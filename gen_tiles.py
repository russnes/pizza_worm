#!/usr/bin/env python3
"""Generate tiles1.bmp: the static 8x8 BG tileset -- a full hand-authored 5x7 pixel font
(A-Z, 0-9, plus '-' and '_'), a blank tile and a wall tile.

The snake's baked tail is NOT drawn from tiles in this sheet.  The play area is a pixel
canvas: every playfield cell owns one tile slot in VRAM, starting at TILE_CANVAS_BASE (the
first index after the static tiles), and psnake.c draws the tail into those slots at run
time.  This script only reserves the index range (via tiles_charmap.h) and defines the
palette -- palette entry 5 is the body green the canvas pixels use.
tile0 = blank (floor/space), tiles1..N = chars, then wall.
"""
from PIL import Image, ImageDraw

FONT5x7 = {
    '0': [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    '1': ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    '2': [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    '3': [".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."],
    '4': ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    '5': ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    '6': ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    '7': ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    '8': [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    '9': [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    'A': [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'B': ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    'C': [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    'D': ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    'E': ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    'F': ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    'G': [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    'H': ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    'I': [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    'J': ["..###", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##.."],
    'K': ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    'L': ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    'N': ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    'O': [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'P': ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    'Q': [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    'R': ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    'S': [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    'T': ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    'U': ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    'V': ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    'W': ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    'X': ["#...#", ".#.#.", "..#..", "..#..", "..#..", ".#.#.", "#...#"],
    'Y': ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    'Z': ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    # punctuation: '-' marks empty high-score slots, '_' is the name-entry cursor
    '-': [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    '_': [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
}
CHARS = list(FONT5x7.keys())

TILE = 8
N_TILES = 1 + len(CHARS) + 1  # blank + chars + wall
COLS = 7
ROWS = (N_TILES + COLS - 1) // COLS
W, H = TILE * COLS, TILE * ROWS

PALETTE = [
    (8, 8, 14),      # 0 background/transparent
    (255, 235, 110), # 1 text fill (pizza yellow, bright for contrast)
    (40, 20, 16),    # 2 text outline (also body dot outline)
    (176, 60, 40),   # 3 wall red
    (232, 200, 150), # 4 wall tan
    (46, 140, 60),   # 5 baked body green - matches sprite body
]

im = Image.new("P", (W, H), 0)
flat = []
for c in PALETTE:
    flat.extend(c)
flat.extend([0, 0, 0] * (256 - len(PALETTE)))
im.putpalette(flat)
draw = ImageDraw.Draw(im)


def cell_origin(idx):
    return (idx % COLS) * TILE, (idx // COLS) * TILE


for i, ch in enumerate(CHARS):
    idx = 1 + i
    ox, oy = cell_origin(idx)
    rows = FONT5x7[ch]
    for ry, row in enumerate(rows):
        for rx, cell in enumerate(row):
            if cell == '#':
                im.putpixel((ox + 1 + rx, oy + ry), 1)

wall_idx = 1 + len(CHARS)
ox, oy = cell_origin(wall_idx)
draw.rectangle([ox, oy, ox + 7, oy + 7], fill=3)
draw.rectangle([ox, oy, ox + 7, oy + 7], outline=2)
draw.rectangle([ox + 1, oy + 1, ox + 6, oy + 6], fill=4)
draw.rectangle([ox + 2, oy + 2, ox + 5, oy + 5], fill=3)

im.save("tiles1.bmp")
print("wrote tiles1.bmp", im.size, "tiles:", N_TILES, "wall_idx:", wall_idx)

ascii_table = [0] * 128
for i, ch in enumerate(CHARS):
    ascii_table[ord(ch)] = 1 + i
ascii_table[ord(' ')] = 0

with open("tiles_charmap.h", "w") as f:
    f.write("// Auto-generated ascii -> tile index table\n")
    f.write("#define TILE_BLANK 0\n")
    f.write(f"#define TILE_WALL {wall_idx}\n")
    f.write(f"#define TILE_CANVAS_BASE {wall_idx + 1}  // first tile slot of the tail canvas (see psnake.c)\n")
    f.write("static const u8 asciiTile[128] = {\n    ")
    f.write(", ".join(str(v) for v in ascii_table))
    f.write("\n};\n\n")
