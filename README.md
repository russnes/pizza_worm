# Pizza Worm (SNES)

A from-scratch classic snake SNES game,
written in C for the 65816 using the [pvsneslib](https://github.com/alekmaul/pvsneslib)
homebrew devkit.

## Features

- Continuous, angle-based movement (turn left/right, not grid-based Snake)
- Very long snakes (up to 504 segments): the most recent 30 segments are
  smooth-moving sprites, and everything older is drawn pixel-for-pixel into the
  background layer as the snake grows past the SNES's sprite limits — see
  "How the long tail works" below
- Pizzas of two sizes/point values, randomly spawned
- 20-entry high score table with on-screen name entry (8 characters: A–Z, 0–9
  and space), **saved to battery-backed SRAM so it survives power-off**
- After every game you land on the high score table (your new entry blinks);
  it can also be opened from the title screen
- Title screen, game-over screen, name-entry screen, high-score screen
- Continuous background music (starts on boot, loops forever)

## Directory contents

| File                  | Purpose                                                          |
|------------------------|-------------------------------------------------------------------|
| `psnake.c`             | Main game source                                                  |
| `hdr.asm`               | SNES ROM header (title, mapping mode, etc.)                       |
| `data.asm`              | Links the compiled graphics/palette data into the ROM              |
| `canvas.inc`            | Tail-canvas WRAM layout and the hand-written 65816 stamping routine (included by `data.asm`) |
| `body_dot.inc`          | Generated: the body-dot footprint used by `canvas.inc`             |
| `check_ram.py`          | Post-link check that fails the build if WRAM sections overlap       |
| `Makefile`              | Build script                                                      |
| `gen_sprites.py`        | Generates `sprites.bmp` (snake head/body/pizza sprite sheet) and `body_dot.inc`; the single definition of the body shape |
| `gen_tiles.py`          | Generates `tiles1.bmp` (font and wall tiles) and reserves the tail canvas' tile range |
| `gen_trig.py`           | Generates `trig_table.h` (sin/cos lookup table for movement)       |

Running `make` regenerates `sprites.bmp`, `tiles1.bmp`, `trig_table.h`,
`sprite_tiles.h`, `body_dot.inc` and `tiles_charmap.h` automatically via the three Python
scripts above, then converts the bitmaps to SNES tile data and compiles the
ROM — you don't need to run the Python scripts yourself unless you want to
edit the generated art/tables directly.

## Prerequisites

- **Python 3** with **Pillow** (`pip install pillow`) — used only at build
  time to generate the sprite sheet, background tileset, and trig table.
- **pvsneslib** (includes the 65816 C compiler, assembler, and linker).
  Prebuilt Linux binaries are the easiest route:

  ```bash
  curl -L -o pvsneslib.zip \
    https://github.com/alekmaul/pvsneslib/releases/download/4.3.0/pvsneslib_430_64b_linux_release.zip
  mkdir pvsneslib_install && cd pvsneslib_install
  unzip ../pvsneslib.zip
  chmod +x pvsneslib/devkitsnes/bin/*
  ```

  This gives you a `pvsneslib/` folder containing `pvsneslib/` (headers/libs)
  and `devkitsnes/` (compiler toolchain). Windows/macOS users, or anyone
  wanting a newer version, should follow pvsneslib's own installation guide:
  <https://github.com/alekmaul/pvsneslib/wiki/Installation>

## Building

From this directory, with the prerequisites above installed:

```bash
export PVSNESLIB_HOME=/path/to/pvsneslib_install/pvsneslib
export PATH=$PATH:$PVSNESLIB_HOME/devkitsnes/bin
make
```

This produces `pizzaworm.sfc` — a standard LoROM SNES ROM you can run in
any SNES emulator (snes9x, Mesen-S, bsnes/higan, RetroArch's snes9x/bsnes
cores) or on real hardware via a flashcart (SD2SNES/FXPak, Super Everdrive,
etc).

`make clean` removes build artifacts (does not delete the generated
`.bmp`/`.h`/`.inc` asset files — delete those manually if you want a fully fresh
regeneration from the Python scripts). The build ends by running
`check_ram.py`, which fails if any WRAM sections overlap.

## Controls

- **D-pad Left/Right** — turn
- **Start** — begin game / confirm on menus
- **Select** — view high scores (from the title screen)
- On the name-entry screen: **Left/Right** move the cursor, **Up/Down**
  cycle the character (hold to auto-repeat), **Start** confirms. The name
  starts as `A` followed by spaces, so a short name needs no cleanup.
- On the high score table: **Start**, **A** or **B** returns to the title

## High scores and saving

Scores are stored in the cartridge's battery-backed SRAM (declared in
`hdr.asm`: `CARTRIDGETYPE $02`, `SRAMSIZE $01` = 2 KB; about 210 bytes are
used). Emulators keep this in a `.srm` file next to the ROM (e.g.
`pizzaworm.srm`); flashcarts such as the FXPak/SD2SNES read the SRAM size from
the ROM header and keep it on the SD card.

The save is written whenever a new high score is entered, and read once at
power-on. Layout (see `saveHighScores()` in `psnake.c`):

| Bytes     | Contents                                              |
|-----------|-------------------------------------------------------|
| 0–3       | magic `"PZSN"`                                        |
| 4         | `NAME_LEN` (8)                                        |
| 5         | `NUM_SCORES` (20)                                     |
| 6–205     | 20 entries of: 8 name bytes, score low, score high    |
| 206–207   | checksum of everything before it (low, high)          |

If the SRAM is blank, corrupt, or was written with different `NAME_LEN` /
`NUM_SCORES` values, the game starts with an empty table (and writes it back)
rather than misreading the data. If no SRAM is available at all (e.g. a
flashcart set up without it) the game still works; scores just last until
power-off.

To **reset the table**, delete the `.srm` file.

## How the long tail works

The SNES can only display 128 hardware sprites at once -- and, more
restrictively, only **32 on any one scanline** -- so a snake that's allowed to
grow can't be *all* sprites. The most recent 30 segments (nearest the head) are
rendered as sprites and move smoothly every frame (`SPRITE_WINDOW`). Once a
segment ages further back than that, it is drawn into the background layer
instead, and since the snake's length only changes when it eats (not with
distance travelled), the *oldest* background segment is erased at the same
rate new ones are added, keeping the total length matched to your score.

**The background layer is a pixel canvas.** Each of the 30×24 playfield cells
owns its own tile in VRAM (`TILE_CANVAS_BASE + cell`), and the tilemap points
at those tiles for the whole game. Every frame the game *stamps* the same 5×5
dot the body sprites use (one definition, in `gen_sprites.py`) at the trail
sample that just aged out of the sprite window, and *un-stamps* the dot at the
sample that just fell off the tail's end. The background tail is therefore the
union of exactly the dots the sprites would have drawn: same width, same
edges, same path, pixel for pixel, with no tile seams to line up and a tail tip
that retreats one pixel at a time instead of one 8×8 cell at a time.

(An earlier version chose each background tile from a library of pre-drawn
connector shapes keyed by where the path entered and left the cell. That can't
encode sub-pixel position or direction, so the baked part came out about a
pixel thinner than the sprite part, with notches, kinks and wandering edges.)

Because dots overlap, a pixel can't simply be cleared when one dot leaves; it
must stay lit while any other dot still covers it. So each pixel keeps a count
of the dots covering it, stored as four bit-planes so that adding or removing a
row of up to 5 pixels is a short ripple-carry over bytes. The counts, and a
second array holding the ready-to-DMA VRAM tiles, live in WRAM bank `$7F` in
the same 4bpp layout as tile data, so one index addresses both. Changed cells
are queued (each at most once) and up to 16 per frame are DMA'd straight from
that array during vblank; a frame normally changes 6–8. The stamping routine
(`canvasStampDot` in `canvas.inc`) is hand-written 65816: the C compiler's
version of the same loop took longer than a whole frame.

Collision with the baked tail is a single pixel lookup: you die if the head's
centre is on a pixel of the baked body, the same rule the sample-position check
applies to the sprite part, so what you see is what kills you.

Edge cases handled: dots that overlap the wall outline at the arena edge are
clipped (the wall tile is opaque, so sprites never showed those pixels
either); the sample cursors are compared modulo 65536 so `snakeAge` wrapping
(after ~18 minutes of continuous play) is harmless; while the snake is younger
than the bake delay the cursor is held at sample 0, so the spawn-point filler
in the trail buffer is never stamped repeatedly; and `MAX_LEN` (504 segments)
is derived from `TRAILSIZE`, because un-stamping a dot re-reads its position
from the trail buffer, which only remembers `TRAILSIZE` samples.

## Performance notes (keeping 60 fps)

The whole game step has to fit in one video frame (about 262 scanlines of CPU
time). Two things burn that budget quickly on a 65816, and both are easy to
reintroduce by accident:

- **There is no divide (or hardware multiply) instruction.** A `u16 %` or `/`
  is a software routine costing hundreds of cycles, and `s32` multiply/divide
  is far worse. In per-frame code use masks and shifts instead
  (`& TRAIL_MASK`, `>> 5`, `div10()`), and keep arithmetic 16-bit where the
  values allow it. This is why `TRAILSIZE` **must be a power of two**.
- **Cost scales with snake length.** `drawSprites()` runs once per body sprite
  per frame (`SPRITE_WINDOW`) and the self-collision loop once per segment
  (`COLLIDE_WINDOW`).
- **The 32-sprites-per-scanline limit is a hard cap, not a hint.** A long snake
  running horizontally puts *every* body sprite on the same scanlines; past 32
  the PPU silently drops the highest-numbered ones (the oldest segments), and
  which ones it drops changes as the snake's shape does -- a hole that
  flickers. Real hardware and accurate emulators (bsnes) do this; a lenient
  emulator may not, which can hide the problem. So `SPRITE_WINDOW + head +
  pizza <= 32` is checked at compile time: it can't be exceeded whatever shape
  the snake makes. Collisions deliberately use a *separate* window
  (`COLLIDE_WINDOW`), so dying doesn't depend on how many sprites we can draw.
  Baking starts `SPRITE_OVERLAP` segments before the sprites run out, because
  sprites sit behind BG tiles and the baked dots simply cover the sprites
  beneath them, keeping the hand-off clear of the moving end of the sprite chain.
- **Hot loops that go through 24-bit pointers belong in assembly.** 816-tcc
  turns every `p[i]` through a far pointer into about ten instructions. The
  C version of the canvas stamping cost more than a whole frame (the game ran
  at ~57% speed); the assembly version costs a few thousand cycles.
- **WRAM layout.** C globals live in `.bss` from `$7E:2000`, but pvsneslib's own
  RAM (console buffer, OAM shadow buffer, ...) is fixed at `$7E:8000` and up, so
  only ~24 KB of `$7E` is available to C. wla-link does **not** warn when `.bss`
  grows into the library's sections; the ROM builds and then corrupts memory.
  Arrays of any size belong in bank `$7F` (as the canvas does, via
  `canvas.inc`), and `check_ram.py` now runs after every link and fails the
  build on any WRAM overlap.

Before this was fixed, every `% TRAILSIZE`/`% 32` in the per-segment and
per-tile paths, the 32-bit diagonal-tile maths, and the number formatting
added up to more than one frame once the snake passed ~45 segments, and the
game dropped to half speed for the rest of the round. Measured in snes9x
(scripted bot, 150–400-segment snakes, several thousand frames), the frame's CPU
work ends at about scanline 120 on average and 184 at worst, against 225 where
vblank starts, with zero lagged frames; the previous tile-based tail averaged
about 91 and peaked at 139, so the pixel canvas costs roughly 30 scanlines more
per frame. The vblank tile uploads finish by scanline ~256 of 261 in the worst
case (typically ~243); `flushUploads()` checks the vblank flag before each tile,
so a burst can never write outside vblank -- the leftovers just go out next
frame. If you add per-frame work, `lag_frame_counter` (pvsneslib) counts frames
that overran.

## Music

Background music plays continuously from the moment the ROM boots (right after
the SPC700 driver is initialised) and loops for the entire session.

The music is supplied as an Impulse Tracker module (`res/pizza_music.it`) and
converted at build time by pvsneslib’s `smconv` tool into a soundbank that the
SM-SPC driver streams to the SPC700.

The supplied WAV (`pizza_snake_full_structure.wav`) is the definitive reference
recording of the intended piece (100 BPM, A-minor centred, with the formal
structure described in the project notes: Intro → A → A' → B → C → Link → A
and seamless loop).  To replace the placeholder module with a faithful
reproduction:

1. Open the WAV (or recreate the progression) in a tracker that can export
   Impulse Tracker modules (Schism Tracker, OpenMPT, etc.).
2. Build instruments/samples that fit the SNES sample budget (BRR, typically
   a few short loops + percussion).
3. Sequence the exact form and chord progression given in the structure table.
4. Export as `res/pizza_music.it` (Instruments mode, not Samples-only) and
   rebuild the ROM.

The current placeholder is the stock “pollen8” example so that the audio
pipeline is fully wired and the game already has continuous music; swap the
IT file for the real arrangement when ready.

## Known limitations

- Music is a placeholder module (see Music section above); the definitive
  arrangement lives in the supplied WAV and still needs to be tracked.
- No two-player mode
- No in-game option to clear the high score table (delete the `.srm` file)
- A power cut in the instant the table is being written fails the checksum and
  resets the table on the next boot
