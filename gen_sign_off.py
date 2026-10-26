#!/usr/bin/env python3
"""Generate the "switched off" version of the neon pizza sign on the splash screen.

The title art (splash.bmp) shows a lit neon pizza-slice sign in the top-right corner.
The splash screen flashes it by swapping a block of the background tilemap between the
lit tiles (already part of splash.pic) and the unlit tiles produced here.

This script reads splash.bmp and writes
  sign_off.bmp   the sign's tile block (SIGN_TW x SIGN_TH tiles) as it looks when switched
                 off. It uses splash.bmp's own palette, so no palette change is needed.
                 The Makefile converts it to sign_off.pic.
  sign_off.h     the location/size of that block, shared with psnake.c.
  --preview      also writes sign_preview.png (lit | off, 6x) for a quick look.

How the "off" picture is made (only palette entries that already exist are used):
  * the neon tubing (slice outline, curly wire, pepperoni rings) becomes dull, dark glass
  * the glow is taken off the wall: glow-tinted pixels and everything inside the slice are
    replaced with brick texture from the same row of plain wall next to the sign, and the
    faint outer edge of the glow (a scatter of brighter bricks the plain wall doesn't have)
    is toned back to the plain wall shades; every other wall pixel keeps its drawn colour
  * the awning under the slice's tip is rebuilt from the neighbouring stripe of the same
    kind, moved to follow the awning's slope (it rises to the right), so the stripes keep
    their colours, shading, top edge and yellow highlight
  * the dark gap under the sign and everything else are left untouched

If splash.bmp is redrawn, adjust the constants below (the seeds must sit on the sign's
yellow tubing).  Needs Pillow only.
"""
import sys
from collections import deque
from PIL import Image

SRC = "splash.bmp"
OUT_BMP = "sign_off.bmp"
OUT_H = "sign_off.h"
OUT_PREVIEW = "sign_preview.png"

# --- where the sign lives, in 8x8 background tiles (splash.map is one tile per cell) --
SIGN_COL, SIGN_ROW = 24, 0       # top-left tile of the block (x=192: takes in the whole orange
                                 # awning stripe beside the sign, which the glow also lights)
SIGN_TW, SIGN_TH = 8, 8          # size of the block in tiles (both must be even: gfx4snes pads
                                 # a bitmap up to a multiple of 16 px, which would scramble the rows)
assert SIGN_TW % 2 == 0 and SIGN_TH % 2 == 0, "SIGN_TW and SIGN_TH must be even"
EDIT = (SIGN_COL * 8, SIGN_ROW * 8, (SIGN_COL + SIGN_TW) * 8, (SIGN_ROW + SIGN_TH) * 8)

# --- how to find the sign's parts (pixel coordinates in splash.bmp) ---------------------
WINDOW = (200, 0, 256, 60)       # search area around the sign (left edge = plain wall)
SEED_OUTLINE = (227, 7)          # a yellow tube pixel on the slice outline
SEED_WIRE = (243, 47)            # a yellow tube pixel on the curly wire under the slice
TUBE_COLORS = (0, 1)             # palette entries of the lit tube (bright / darker yellow)
RING_COLOR = 2                   # the pepperoni rings are orange tubes inside the slice
TINT_COLORS = (0, 1, 2, 3, 4, 5)  # palette entries only the glow produces on the wall
HALO_COLOR = 8                   # the brighter dark red the glow fades out through.  Plain wall
                                 # has none, so every one in the sign's block is glow
FRINGE_COLOR = 3                 # stray green anti-alias pixels along the tubes
GLOW_REACH = 10                  # tinted wall pixels this close (px) to the tubes get replaced
WALL_YMAX = 49                   # the wall ends here; the awning's top edge is at y=49..52
AWNING_BOX = (200, 49, 222, 63)  # awning under the slice's tip (x0, y0, x1, y1); right of it
                                 # the dark gap under the sign takes over
STRIPE_PERIOD = 20               # the awning's stripes repeat every 20 px
STRIPE_COLORS = (2, 4, 5)        # the orange stripes' shades, used to find their top edge
STRIPE_SHIFT = 2                 # the awning slopes up to the right: near its top edge a stripe
                                 # sits about this many rows higher than the one a period to its
                                 # left (measured per column; this is the fallback) ...
BOTTOM_SHIFT, SHIFT_SPLIT = 1, 58   # ... and this many from row SHIFT_SPLIT down (the bottom
                                 # edge and its yellow highlight slope less)
HIGHLIGHT_ROW = 61               # the stripes' yellow highlight starts here; its edges also sit
                                 # a pixel to the side (they are slanted), measured below
STRAY_COLOR = 3                  # lone green pixels in the awning art: never copied
ORANGE_X1 = 211                  # the orange stripe beside the tip spans x < this; its edges can be
                                 # measured.  Right of it the dark-red stripe under the tip has
                                 # no readable edge (tube and glow cover it): the defaults apply
PLAIN_8 = (9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10)  # wall mix: ~55% / ~45%
NAVY = (6, 11, 13)               # dark-gap colours, never touched around the wire
WALL_MIN_RUN = 8                 # shortest strip of clean brick worth tiling
BRICK_X0 = 182                   # plain brick starts here (left of it is the "PIZZA" lettering)
SKY_ROWS, SKY_X0 = 10, 206       # the top rows are black/navy left of x=206; bricks start there

# --- the unlit look, as palette entries already in splash.bmp ---------------------------
OFF_TUBE = {0: 4, 1: 5}          # dull red-orange / dark brown glass
OFF_RING = 5

N8 = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]
N4 = N8[:4]


def inwin(x, y):
    return WINDOW[0] <= x < WINDOW[2] and WINDOW[1] <= y < WINDOW[3]


def editable(p):
    """Only pixels inside the swapped tile block may change."""
    return EDIT[0] <= p[0] < EDIT[2] and EDIT[1] <= p[1] < EDIT[3]


def component(seed, ok, neighbours, within=inwin):
    seen, queue = set(), deque([seed])
    while queue:
        p = queue.popleft()
        if p in seen or not within(*p) or not ok(p):
            continue
        seen.add(p)
        queue.extend((p[0] + dx, p[1] + dy) for dx, dy in neighbours)
    return seen


def make_off(lit):
    """Return a copy of the lit splash image with the sign switched off."""
    ref, off = lit.load(), lit.copy()
    out = off.load()

    for name, seed in (("SEED_OUTLINE", SEED_OUTLINE), ("SEED_WIRE", SEED_WIRE)):
        if ref[seed] not in TUBE_COLORS:
            sys.exit(f"gen_sign_off.py: {name} {seed} is not on the sign's yellow tubing "
                     f"in {SRC}; update the constants at the top of the script")

    outline = component(SEED_OUTLINE, lambda p: ref[p] in TUBE_COLORS, N8)
    wire = component(SEED_WIRE, lambda p: ref[p] in TUBE_COLORS, N8)
    tubes = outline | wire

    # inside of the slice: whatever the outside can't reach without crossing the outline
    # (the outline is thickened by 1px first so a hairline gap can't leak)
    barrier = {(x + dx, y + dy) for x, y in outline for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
    outside, queue = set(), deque()
    x0, y0, x1, y1 = WINDOW
    for x in range(x0, x1):
        queue += [(x, y0), (x, y1 - 1)]
    for y in range(y0, y1):
        queue += [(x0, y), (x1 - 1, y)]
    while queue:
        p = queue.popleft()
        if p in outside or p in barrier or not inwin(*p):
            continue
        outside.add(p)
        queue.extend((p[0] + dx, p[1] + dy) for dx, dy in N4)
    interior = {(x, y) for x in range(x0, x1) for y in range(y0, y1)
                if (x, y) not in outside and (x, y) not in barrier}
    rings = {p for p in interior if ref[p] == RING_COLOR}
    if len(outline) < 100 or len(interior) < 100 or not rings:
        sys.exit("gen_sign_off.py: couldn't make out the sign in splash.bmp "
                 f"(outline {len(outline)} px, interior {len(interior)} px, rings {len(rings)} px)")

    # distance (in px, 8-way) from the tubes
    reach = GLOW_REACH
    dist = {p: 0 for p in tubes}
    queue = deque(dist)
    while queue:
        p = queue.popleft()
        if dist[p] >= reach:
            continue
        for dx, dy in N8:
            n = (p[0] + dx, p[1] + dy)
            if n not in dist and inwin(*n):
                dist[n] = dist[p] + 1
                queue.append(n)

    lit_parts = tubes | rings

    # 1. awning geometry.  It slopes up to the right, so a stripe's top edge is a few rows
    #    higher than the edge of the clean stripe one period to the left.  Measure both edges
    #    so the rebuilt stripes keep the top edge exactly where the lit picture has it.
    ax0, ay0, ax1, ay1 = AWNING_BOX

    def top_of_stripe(x):
        for y in range(ay0, ay0 + 8):
            if ref[x, y] in STRIPE_COLORS:
                return y
        return None

    # (the stripes' edges are slanted, so near a stripe's corners the matching clean column
    #  can be a pixel or two to the side: take the nearest column whose edge lines up)
    awning_top, awning_shift, awning_src = {}, {}, {}
    for x in range(ax0, ax1):
        t, shift, src_x = (top_of_stripe(x) if x < ORANGE_X1 else None), STRIPE_SHIFT, x - STRIPE_PERIOD
        if t is not None:
            for k in (0, 1, -1, 2, -2):
                src = top_of_stripe(x - STRIPE_PERIOD + k)
                if src is not None and 1 <= src - t <= 3:
                    shift, src_x = src - t, x - STRIPE_PERIOD + k
                    break
            else:
                t = None                                  # no clean reading: use the slope
        awning_top[x] = t if t is not None else (ay0 + 1 if x < 208 else ay0)
        awning_shift[x], awning_src[x] = shift, src_x

    # the slanted edges also move sideways between the top and the bottom of a stripe: pick
    # the sideways offset for the bottom band that lines up the yellow highlight pattern
    def lit_highlight(x, y):
        return ref[x, y] in (0, 1)
    k_bottom = max((0, 1, -1), key=lambda k: sum(
        lit_highlight(x, ay1 - 1) == lit_highlight(x - STRIPE_PERIOD + k, ay1 - 1 + BOTTOM_SHIFT)
        for x in range(ax0, ORANGE_X1)))

    def awning(p):
        return ax0 <= p[0] < ax1 and awning_top[p[0]] <= p[1] < ay1

    # 2. wall: take the glow off.  Only pixels the glow tinted are replaced (plus everything
    #    inside the slice); every other pixel is genuine wall and stays exactly as drawn.
    wall = {p for p, d in dist.items()
            if p[1] <= WALL_YMAX and p not in lit_parts and editable(p) and not awning(p)
            and d <= GLOW_REACH and ref[p] in TINT_COLORS}
    wall |= {p for p in interior if p[1] <= WALL_YMAX and p not in lit_parts and editable(p) and not awning(p)}
    # Fill from the same row: tile the stretch of plain brick just left of the sign.  Using
    # the same row keeps horizontal features (the mortar band at y=47..49) running through.
    rows = {}
    for p in wall:
        rows.setdefault(p[1], []).append(p[0])
    for y, xs in rows.items():
        end = min(xs + [x for (x, yy) in lit_parts if yy == y]) - 1   # plain brick ends where the glow starts
        start = max(BRICK_X0, end - 24)
        if y < SKY_ROWS:
            start = max(start, SKY_X0)
        if end - start + 1 < WALL_MIN_RUN:
            start = max(BRICK_X0, end - WALL_MIN_RUN + 1)
        run = end - start + 1
        for x in xs:
            out[x, y] = ref[start + (x - start) % run, y]
    # The far edge of the glow is faint: it only turns some bricks into the brighter dark red
    # (8), up to ~25 px from the sign.  The genuine wall has none, so swap them all for the
    # plain wall shades.
    for y in range(EDIT[1], WALL_YMAX + 1):
        for x in range(EDIT[0], EDIT[2]):
            p = (x, y)
            if out[p] == HALO_COLOR and p not in lit_parts and not awning(p):
                out[p] = PLAIN_8[(x * 7 + y * 13 + (x * y) % 5) % len(PLAIN_8)]

    # 3. awning: the slice's tip hangs over it.  The awning is a pattern of vertical stripes
    #    (period STRIPE_PERIOD) shaded from the top down, so rebuild the stripes under and
    #    beside the tip from the neighbouring stripe of the same kind one period to the left,
    #    moved up/down by the slope so the stripe keeps its shape.
    for x in range(ax0, ax1):
        for y in range(awning_top[x], ay1):
            p = (x, y)
            if editable(p) and p not in lit_parts and ref[p] not in NAVY and ref[p] != FRINGE_COLOR:
                if y < SHIFT_SPLIT:
                    sx, sy = awning_src[x], y + awning_shift[x]
                elif y < HIGHLIGHT_ROW:
                    sx, sy = x - STRIPE_PERIOD, y + BOTTOM_SHIFT
                else:
                    sx, sy = x - STRIPE_PERIOD + k_bottom, y + BOTTOM_SHIFT
                if ref[sx, sy] == STRAY_COLOR:           # a stray pixel of the clean stripe:
                    sy += 1 if ref[sx, sy + 1] != STRAY_COLOR else -1   # take its neighbour instead
                out[p] = ref[sx, sy]
    # 4. green anti-alias fringe hugging the tubes -> whatever surrounds it
    for p, d in dist.items():
        if editable(p) and d <= 2 and p not in tubes and ref[p] == FRINGE_COLOR and out[p] == FRINGE_COLOR:
            votes = {}
            for dx, dy in N8:
                n = (p[0] + dx, p[1] + dy)
                if inwin(*n) and n not in tubes and ref[n] not in TUBE_COLORS + (FRINGE_COLOR,):
                    votes[ref[n]] = votes.get(ref[n], 0) + 1
            out[p] = max(votes, key=votes.get) if votes else 6
    # 5. the tubes themselves: dull unlit glass
    for p in tubes:
        out[p] = OFF_TUBE[ref[p]]
    for p in rings:
        out[p] = OFF_RING

    # everything is cut to whole tiles and all of them get swapped, so nothing may change
    # outside the block (or the lit and unlit pictures would no longer match up)
    w, h = lit.size
    for y in range(h):
        for x in range(w):
            if ref[x, y] != out[x, y] and not (EDIT[0] <= x < EDIT[2] and EDIT[1] <= y < EDIT[3]):
                sys.exit(f"gen_sign_off.py: pixel ({x},{y}) changes outside the sign's tile "
                         f"block {EDIT}; widen SIGN_COL/SIGN_ROW/SIGN_TW/SIGN_TH")
    return off


def main():
    lit = Image.open(SRC)
    if lit.mode != "P" or lit.size != (256, 224):
        sys.exit(f"gen_sign_off.py: {SRC} must be a 256x224 palette (8-bit) bitmap")
    lit = lit.copy()
    off = make_off(lit)

    block = off.crop(EDIT)
    block.putpalette(lit.getpalette())       # same palette as the splash: indices carry over
    block.save(OUT_BMP)

    with open(OUT_H, "w") as f:
        f.write("/* Generated by gen_sign_off.py -- do not edit. */\n"
                "#ifndef SIGN_OFF_H\n#define SIGN_OFF_H\n\n"
                "/* The flashing neon sign: a block of the splash tilemap (one tile per cell) */\n"
                f"#define SIGN_COL {SIGN_COL}   /* tilemap column of the block's top-left tile */\n"
                f"#define SIGN_ROW {SIGN_ROW}   /* tilemap row of the block's top-left tile */\n"
                f"#define SIGN_TW  {SIGN_TW}   /* block width in tiles */\n"
                f"#define SIGN_TH  {SIGN_TH}   /* block height in tiles; sign_off.pic holds SIGN_TW*SIGN_TH tiles, row by row */\n\n"
                "#endif\n")
    changed = sum(1 for y in range(224) for x in range(256) if lit.getpixel((x, y)) != off.getpixel((x, y)))
    print(f"wrote {OUT_BMP} ({block.size[0]}x{block.size[1]}) and {OUT_H}; {changed} pixels differ from the lit sign")

    if "--preview" in sys.argv:
        s, box = 6, (EDIT[0] - 8, 0, 256, EDIT[3])
        a = lit.convert("RGB").crop(box)
        b = off.convert("RGB").crop(box)
        w, h = (box[2] - box[0]) * s, (box[3] - box[1]) * s
        sheet = Image.new("RGB", (w * 2 + 12, h), (40, 40, 40))
        sheet.paste(a.resize((w, h), Image.NEAREST), (0, 0))
        sheet.paste(b.resize((w, h), Image.NEAREST), (w + 12, 0))
        sheet.save(OUT_PREVIEW)
        print(f"wrote {OUT_PREVIEW}")


if __name__ == "__main__":
    main()
