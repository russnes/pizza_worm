#!/usr/bin/env python3
"""Generate sprites.bmp (+ sprite_tiles.h, body_dot.inc).

Snake head/body are simple classic-snake-style dots: no face/rotation, no per-segment
direction (avoids jitter -- position-only updates every frame).

The pizzas come in five sizes, all pepperoni.  Each one is drawn by the little renderer
below (crust ring, sauce, melted cheese, shaded pepperoni) rather than pixel-by-pixel by
hand, so the five sizes share one look:

    name           pizza     sprite(s)
    pizza_small     8 px     one  8x8   OBJ
    pizza_medium   12 px     one 16x16  OBJ  (centred in the box)
    pizza_big      16 px     one 16x16  OBJ
    pizza_large    24 px     2x2 of 16x16 OBJs = a 32x32 box (centred)
    pizza_giant    32 px     2x2 of 16x16 OBJs = a 32x32 box

The hardware only has 8x8 and 16x16 sprites in this project's OBJ size mode, so the two
biggest pizzas are drawn as one contiguous 32x32 picture on the sheet and psnake.c puts it
on screen as four 16x16 sprites (TL, TR, BL, BR = tile + 0, 2, 32, 34).

Sheet layout: 16 tile columns x 4 tile rows (128x32 px).  A tile at (col, row) lands in
VRAM at OBJ tile number row*16 + col, which is what the SPR_* defines below hold.

Run with --preview to also write pizza_preview.png (the pizzas on the arena floor, 8x).
"""
import math
import random
import sys
from PIL import Image, ImageDraw

TILE = 8
COLS, ROWS = 16, 4
W, H = TILE * COLS, TILE * ROWS          # 128 x 32

# name, first tile column, first tile row, box size in px.  Every name keeps its box
# contiguous on the sheet, so a 32x32 box is simply four 16x16 sprites in a 2x2 block.
LAYOUT = [
    ("head",         0, 0, 8),
    ("pizza_big",    1, 0, 16),
    ("pizza_small",  3, 0, 8),
    ("body",         4, 0, 8),
    ("pizza_medium", 5, 0, 16),
    ("pizza_large",  7, 0, 32),
    ("pizza_giant", 11, 0, 32),
]

PALETTE = [
    (255, 0, 255),   # 0  transparent
    (20, 20, 24),    # 1  outline/black (snake head)
    (94, 220, 90),   # 2  head bright green
    (46, 140, 60),   # 3  body green
    (170, 108, 50),  # 4  crust mid brown
    (122, 72, 32),   # 5  crust dark brown (shadow side)
    (226, 64, 38),   # 6  tomato sauce red
    (252, 222, 100), # 7  cheese yellow
    (124, 26, 34),   # 8  pepperoni dark (shadow side)
    (182, 38, 40),   # 9  pepperoni red (deeper than the sauce, so the two read apart)
    (224, 98, 82),   # 10 pepperoni highlight
    (230, 152, 52),  # 11 cheese browned / orange
    (206, 142, 70),  # 12 crust light brown (lit side)
    (88, 50, 22),    # 13 deep brown (outline)
    (255, 244, 182), # 14 cheese highlight (pale)
    (190, 44, 32),   # 15 sauce dark (shadow side)
]

im = Image.new("P", (W, H), 0)
flat = []
for c in PALETTE:
    flat.extend(c)
flat.extend([0, 0, 0] * (256 - len(PALETTE)))
im.putpalette(flat)
draw = ImageDraw.Draw(im)


def origin(name):
    for n, col, row, size in LAYOUT:
        if n == name:
            return col * TILE, row * TILE, size
    raise KeyError(name)


# ----------------------------------------------------------------------------------
# Pizza renderer
# ----------------------------------------------------------------------------------
def disc(diam, shave=0.0):
    """Pixels of a filled circle of the given diameter, as {(x, y): (dx, dy)} where dx/dy
    is the offset of the pixel centre from the circle centre.  `shave` shrinks the radius
    a touch (a 3px disc is a rounded plus instead of a hard square)."""
    r = diam / 2.0
    out = {}
    for y in range(diam):
        for x in range(diam):
            dx, dy = x + 0.5 - r, y + 0.5 - r
            if math.hypot(dx, dy) <= r - shave:
                out[(x, y)] = (dx, dy)
    return out


def depth_map(pix):
    """Distance from each pixel of `pix` to the nearest pixel outside it (exact, so the
    rings it produces are round rather than diamond or square shaped)."""
    xs = [p[0] for p in pix]
    ys = [p[1] for p in pix]
    x0, x1, y0, y1 = min(xs) - 3, max(xs) + 3, min(ys) - 3, max(ys) + 3
    outside = [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1) if (x, y) not in pix]
    return {p: min(math.hypot(p[0] - q[0], p[1] - q[1]) for q in outside) for p in pix}


def band_of(depth, thin=False):
    """Ring number for a depth: ring 0 is the outermost pixel row, ring 1 the next...
    thin=True makes every ring a true 1px line at any angle (the default is a little
    fatter on the diagonals)."""
    if thin:
        return max(0, int(math.ceil(depth - 0.05)) - 1)
    return max(0, int(math.ceil(depth - 0.5)) - 1)


def draw_pepperoni(canvas, allowed, ox, oy, d):
    """One shaded pepperoni slice of diameter d with its top-left at (ox, oy).  Light comes from
    the upper left: dark-red rim on the lower right, a bright highlight upper left.
    Only paints over pixels in `allowed` (the cheese), so a slice that pokes under the
    crust or sauce is neatly clipped, like a real one."""
    shape = disc(d, shave=0.05)
    dep = depth_map(shape)
    hi = None
    if d >= 4:
        # highlight: the interior pixel nearest the upper-left, a little in from the rim
        want = (-0.28 * d, -0.28 * d)
        hi = min(shape, key=lambda p: (shape[p][0] - want[0]) ** 2 + (shape[p][1] - want[1]) ** 2)
    for p, (dx, dy) in shape.items():
        q = (ox + p[0], oy + p[1])
        if q not in allowed:
            continue
        rim = band_of(dep[p]) == 0
        if p == hi:
            col = 10
        elif rim and (dx + dy) > 0.15 * d:
            col = 8                      # shaded edge, lower right
        else:
            col = 9
        canvas[q] = col
    if d >= 7:
        # a second, smaller glint on the big slices
        for p in shape:
            if p == (hi[0] + 1, hi[1]) or p == (hi[0], hi[1] + 1):
                q = (ox + p[0], oy + p[1])
                if q in allowed and canvas.get(q) == 9:
                    canvas[q] = 10


def render_pizza(diam, bands, sauce_bands, pepperoni, spots, seed, wobble=0.0, patches=(0.0, 3)):
    """Render a pizza into {(x, y): colour index} on a diam x diam grid.

    bands        colour for each ring of crust from the outside in, as (upper-left,
                 lower-right) pairs -- the pair is what makes the crust look lit from the
                 upper left
    sauce_bands  how many rings of tomato sauce sit between the crust and the cheese
    pepperoni    list of (x, y, d) slices: top-left pixel and diameter
    spots        number of browned-cheese spots (and as many pale highlights)
    wobble       chance that each pixel of cheese touching the sauce ring turns to sauce
                 too, so the cheese edge is ragged instead of a perfect circle
    patches      (cover, size): sauce showing through the cheese. `cover` is the fraction of
                 the open cheese area it takes up; it is spread out evenly as many small,
                 separate patches (short strokes of at most `size` pixels, never touching
                 each other), so it can't pool into one big slab
    """
    pix = disc(diam)
    dep = depth_map(pix)
    rng = random.Random(seed)
    canvas = {}
    ring = {}                              # pixel -> ring number
    cheese = set()
    n_crust = len(bands)
    first_cheese = n_crust + sauce_bands
    for p, (dx, dy) in pix.items():
        b = band_of(dep[p], thin=True)
        ring[p] = b
        if b < n_crust:
            ul, lr = bands[b]
            canvas[p] = ul if (dx + dy) < 0 else lr
        elif b < first_cheese:
            canvas[p] = 6 if (dx + dy) < 0.25 * diam else 15
        else:
            canvas[p] = 7
            cheese.add(p)
    inside = set(cheese)                   # everything inside the sauce ring

    # Where the pepperoni will sit (plus a one pixel halo), so sauce goes where it shows.
    pep_px = set()
    for (x, y, d) in pepperoni:
        for (qx, qy) in disc(d, 0.05):
            pep_px.add((x + qx, y + qy))
    near_pep = set()
    for (qx, qy) in pep_px:
        for ddx in (-1, 0, 1):
            for ddy in (-1, 0, 1):
                near_pep.add((qx + ddx, qy + ddy))

    # Ragged inner edge of the sauce ring: some of the outermost cheese pixels are sauce.
    if sauce_bands and wobble:
        for p in sorted(cheese):
            if ring[p] == first_cheese and p not in near_pep and rng.random() < wobble:
                canvas[p] = 6
                cheese.discard(p)

    # Sauce showing through the cheese, spread evenly: each new patch starts at the open spot
    # farthest from the existing ones (best-candidate sampling), then grows into a short
    # stroke of 1..size pixels that never touches another patch or the sauce ring.
    cover, size = patches
    if cover:
        # keep the sauce off the four edges of every slice (so each slice stays crisp against
        # cheese); it may still touch one at a corner
        edge_of_pep = set(pep_px)
        for (qx, qy) in pep_px:
            for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                edge_of_pep.add((qx + ddx, qy + ddy))
        base = [p for p in sorted(cheese) if p not in edge_of_pep]
        target = int(round(cover * len(base)))
        owner = {p: -1 for p in pix if canvas[p] in (6, 15) and p not in cheese}   # ring + ragged edge
        allowed = set(base)
        dirs = [(1, 0), (-1, 0), (0, 1), (0, -1)]
        around = [(a, b) for a in (-1, 0, 1) for b in (-1, 0, 1)]
        seeds, sizes, covered = [], [], 0
        for _ in range(400):
            if covered >= target:
                break
            free = [p for p in base if p not in owner and all((p[0] + a, p[1] + b) not in owner for a, b in around)]
            if not free:
                break
            pick = rng.sample(free, min(12, len(free)))
            p = max(pick, key=lambda q: min([(q[0] - t[0]) ** 2 + (q[1] - t[1]) ** 2 for t in seeds] or [0]))
            pid = len(seeds)
            seeds.append(p)
            want = rng.randint(max(1, size // 2), size)
            cells, heading = [p], rng.choice(dirs)
            owner[p] = pid
            while len(cells) < want:
                opts = []
                for c in cells:
                    for d in dirs:
                        q = (c[0] + d[0], c[1] + d[1])
                        if q in allowed and q not in owner and all(owner.get((q[0] + a, q[1] + b), pid) == pid for a, b in around):
                            opts.append((q, d))
                if not opts:
                    break
                straight = [o for o in opts if o[1] == heading]
                q, heading = rng.choice(straight) if straight and rng.random() < 0.65 else rng.choice(opts)
                cells.append(q)
                owner[q] = pid
            sizes.append(len(cells))
            covered += len(cells)
        for p, pid in owner.items():
            if pid >= 0:
                canvas[p] = 6
                cheese.discard(p)
        # the lower-right end of each stroke is a little darker, like the sauce ring
        for p, pid in owner.items():
            if pid >= 0 and sizes[pid] >= 2 and (p[0] + 1, p[1]) not in owner and (p[0], p[1] + 1) not in owner:
                canvas[p] = 15

    # Browned cheese blotches and pale shine, placed pseudo-randomly but reproducibly.
    cells = sorted(cheese)
    rng.shuffle(cells)
    for p in cells[:spots]:
        canvas[p] = 11
    for p in cells[spots:spots * 2]:
        canvas[p] = 14

    # Pepperoni sits on top of the cheese and sauce, tucked under the sauce ring.
    for (x, y, d) in pepperoni:
        draw_pepperoni(canvas, inside, x, y, d)
    return canvas


# Per-size recipes.  `bands` are listed outside-in.  Pepperoni are (x, y, d): top-left
# pixel and diameter, measured from the pizza's own top-left corner.
RECIPES = {
    "pizza_small": dict(
        diam=8,
        bands=[(12, 4)],
        sauce_bands=0,
        pepperoni=[(2, 2, 2), (5, 3, 2)],
        spots=0, patches=(0.12, 2), seed=1),
    "pizza_medium": dict(
        diam=12,
        bands=[(12, 4)],
        sauce_bands=1,
        pepperoni=[(5, 2, 3), (7, 7, 2), (2, 6, 2)],
        spots=2, patches=(0.18, 3), seed=30),
    "pizza_big": dict(
        diam=16,
        bands=[(12, 4)],
        sauce_bands=1, wobble=0.15,
        pepperoni=[(2, 8, 3), (6, 11, 3), (6, 6, 3), (11, 6, 3), (8, 2, 3)],
        spots=3, patches=(0.22, 3), seed=12),
    "pizza_large": dict(
        diam=24,
        bands=[(4, 5), (12, 4)],
        sauce_bands=1, wobble=0.35,
        pepperoni=[(15, 5, 4), (11, 11, 4), (3, 9, 4), (10, 16, 4), (8, 4, 4), (7, 13, 3),
                   (18, 12, 3)],
        spots=6, patches=(0.25, 4), seed=5),
    "pizza_giant": dict(
        diam=32,
        bands=[(4, 5), (12, 4)],
        sauce_bands=1, wobble=0.4,
        pepperoni=[(6, 10, 5), (9, 21, 5), (14, 14, 5), (15, 4, 5), (22, 16, 4), (6, 17, 4),
                   (21, 22, 4), (15, 22, 4), (23, 8, 3), (9, 4, 3)],
        spots=10, patches=(0.28, 5), seed=5),
}


def draw_pizza(name):
    ox, oy, size = origin(name)
    rec = RECIPES[name]
    diam = rec["diam"]
    canvas = render_pizza(diam, rec["bands"], rec["sauce_bands"], rec["pepperoni"],
                          rec["spots"], rec["seed"], rec.get("wobble", 0.0), rec.get("patches", (0.0, 3)))
    off = (size - diam) // 2             # centre the pizza in its sprite box
    for (x, y), col in canvas.items():
        im.putpixel((ox + off + x, oy + off + y), col)


def draw_dot(name, fill, outline=1, r=2):
    ox, oy, size = origin(name)
    cx, cy = ox + size // 2, oy + size // 2
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline)


# The body dot: a 5x5 block with the four corner pixels cut off, one bitmask per row
# (bit 4 = leftmost pixel).  This is the ONE definition of the snake's body shape: the
# body sprite is drawn from it here, and the same rows are emitted into sprite_tiles.h
# (bodyDotBits) so psnake.c stamps the identical dot into the background layer when it
# bakes the tail.  Sprite part and baked part can therefore never drift apart.
BODY_DOT_ROWS = [0b01110, 0b11111, 0b11111, 0b11111, 0b01110]


def draw_body_dot(name, fill):
    ox, oy, size = origin(name)
    cx, cy = ox + size // 2, oy + size // 2
    for k, bits in enumerate(BODY_DOT_ROWS):
        for b in range(5):
            if bits & (0x10 >> b):
                im.putpixel((cx - 2 + b, cy - 2 + k), fill)


draw_dot("head", fill=2)
for pizza in RECIPES:
    draw_pizza(pizza)
draw_body_dot("body", fill=3)

im.save("sprites.bmp")
print("wrote sprites.bmp", im.size)

if "--preview" in sys.argv:
    scale = 8
    names = ["pizza_small", "pizza_medium", "pizza_big", "pizza_large", "pizza_giant"]
    pad = 6
    sizes = [origin(n)[2] for n in names]
    pw = sum(sizes) + pad * (len(names) + 1) + 16
    ph = max(sizes) + pad * 2
    prev = Image.new("RGB", (pw, ph), (8, 8, 14))
    x = pad
    rgb = im.convert("RGBA")
    pal = im.getpalette()
    for n, s in zip(names, sizes):
        ox, oy, _ = origin(n)
        crop = im.crop((ox, oy, ox + s, oy + s))
        for yy in range(s):
            for xx in range(s):
                v = crop.getpixel((xx, yy))
                if v != 0:
                    prev.putpixel((x + xx, pad + (ph - 2 * pad - s) // 2 + yy), tuple(pal[v * 3:v * 3 + 3]))
        x += s + pad
    # snake head + body for scale
    for n in ("head", "body"):
        ox, oy, s = origin(n)
        for yy in range(s):
            for xx in range(s):
                v = im.getpixel((ox + xx, oy + yy))
                if v != 0:
                    prev.putpixel((x + xx, ph - pad - s + yy), tuple(pal[v * 3:v * 3 + 3]))
        x += 8
    prev = prev.resize((pw * scale, ph * scale), Image.NEAREST)
    prev.save("pizza_preview.png")
    print("wrote pizza_preview.png", prev.size)

names = {
    "SPR_HEAD": "head",
    "SPR_PIZZA_BIG": "pizza_big",
    "SPR_PIZZA_SMALL": "pizza_small",
    "SPR_BODY": "body",
    "SPR_PIZZA_MEDIUM": "pizza_medium",
    "SPR_PIZZA_LARGE": "pizza_large",
    "SPR_PIZZA_GIANT": "pizza_giant",
}
with open("sprite_tiles.h", "w") as f:
    f.write("// Auto-generated OBJ tile numbers: the OBJ tile number of a sprite sheet tile at\n")
    f.write("// (col, row) is row*16 + col.  The 32x32 pizzas (LARGE, GIANT) are four 16x16 sprites;\n")
    f.write("// the define is the top-left one, the others are +2 (TR), +32 (BL) and +34 (BR).\n")
    for define, name in names.items():
        ox, oy, _ = origin(name)
        f.write(f"#define {define} {(oy // TILE) * 16 + ox // TILE}\n")

# bodyDotBits[row*8 + off]: the dot's row mask as it lands in a 16-bit window whose high byte
# is the 8px background cell the dot's left edge falls in (bit 7 = that cell's leftmost pixel)
# and whose low byte is the next cell to the right, for a left edge `off` pixels into the cell.
# Written as an assembly include: the tail-stamping routine (canvasStampDot in data.asm) is
# hand-written 65816 -- the C compiler's code for it was several times too slow for 60 fps.
with open("body_dot.inc", "w") as f:
    f.write("; Auto-generated by gen_sprites.py -- do not edit.\n")
    f.write("; Body dot footprint, pre-shifted for every horizontal offset: bodyDotBits[row*8 + off].\n")
    f.write("; Rows: %s (bit 4 = leftmost pixel)\n" % ", ".join("%%%s" % format(b, "05b") for b in BODY_DOT_ROWS))
    f.write("bodyDotBits:\n")
    for bits in BODY_DOT_ROWS:
        f.write("    .dw " + ", ".join("$%04X" % (bits << (11 - off)) for off in range(8)) + "\n")
