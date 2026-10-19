/*---------------------------------------------------------------------------------
    PIZZA SNAKE - SNES
---------------------------------------------------------------------------------*/
#include <snes.h>
#include <string.h>

#include "trig_table.h"
#include "tiles_charmap.h"
#include "sprite_tiles.h"
#include "res/soundbank.h"

extern char SOUNDBANK__;

extern char gfxsprites, gfxsprites_end;
extern char palsprites, palsprites_end;
extern char gfxwalls, gfxwalls_end;
extern char palwalls, palwalls_end;
extern char gfxsplash, gfxsplash_end;
extern char palsplash, palsplash_end;
extern char mapsplash, mapsplash_end;

/* Helper that keeps the SPC700 music engine fed and then waits for VBlank.
   Must be called every frame or the music will stutter / stop. */
static void waitVBlank(void)
{
    spcProcess();
    WaitForVBlank();
}

//---------------------------------------------------------------------------------
// Arena bounds (pixel space)
//
// The walls are a ring of 8x8 tiles (tilemap rows 2 and 27, columns 0 and 31 --
// see drawWalls()), so the tile interior is x 8..247, y 24..215. Every wall tile
// has a 1px near-black outline on all four sides (41,16,16 against the 8,8,8
// floor): effectively invisible, so the wall *looks* 1px thinner than it is.
// That innermost outline pixel is therefore treated as free space (WALL_BORDER),
// and the snake only dies when it would overlap the bright part of the wall.
//
// The head, like every body segment, is a 5x5 dot reaching HEAD_R = 2px out from
// its centre in each direction, and it's the head's *centre* that is tested
// against the ARENA_* bounds. Letting the centre go to (free edge + HEAD_R) puts
// the dot's outer edge exactly on the last free pixel; one pixel further would
// overlap the visible wall, and that is when the snake dies. (These were
// 14/242/30/210 originally, which killed the snake 3-4px before it visibly
// reached the wall.)
//
// The bounds still keep the head's centre inside the tile interior (x 8..247,
// y 24..215), so the head's tile is never a wall tile and baking the tail can
// never overwrite (or erase) a wall.
#define WALL_BORDER  1
#define PLAY_LEFT    (8 - WALL_BORDER)
#define PLAY_RIGHT   (247 + WALL_BORDER)
#define PLAY_TOP     (24 - WALL_BORDER)
#define PLAY_BOTTOM  (215 + WALL_BORDER)
#define HEAD_R       2
#define ARENA_LEFT   (PLAY_LEFT + HEAD_R)
#define ARENA_RIGHT  (PLAY_RIGHT - HEAD_R)
#define ARENA_TOP    (PLAY_TOP + HEAD_R)
#define ARENA_BOTTOM (PLAY_BOTTOM - HEAD_R)

/* Pizzas spawn in the region they always have (kept clear of the walls for the
   16x16 big pizza); deliberately NOT tied to the death bounds above, so fixing
   the walls doesn't move pizzas or change the random sequence. */
#define SPAWN_LEFT   14
#define SPAWN_RIGHT  242
#define SPAWN_TOP    30
#define SPAWN_BOTTOM 210

#define TRAILSIZE    1024  /* MUST be a power of two: indices wrap with & TRAIL_MASK, not %.
                               The 65816 has no divide instruction, so a u16 % is a slow
                               software routine -- it used to run ~100x per frame and was
                               what made the game drop to half speed past ~45 segments. */
#define TRAIL_MASK   (TRAILSIZE - 1)
/* The SNES draws at most 32 sprites on any one scanline, and a long snake
   running horizontally puts *every* body sprite on the same scanlines. Past 32
   the PPU silently drops the highest-numbered sprites (here: the oldest
   segments) -- a hole in the snake that appears, vanishes and flickers as the
   shape changes, on real hardware and accurate emulators (a lenient emulator
   may hide it). So the sprite budget must be a hard guarantee, not a hope:
   head + pizza + SPRITE_WINDOW body sprites <= 32, whatever the snake's shape. */
#define SPRITE_WINDOW 30   /* most recent segments rendered as smooth sprites; everything
                               older is baked onto the BG layer (see stepSnake()) */
#if SPRITE_WINDOW + 2 > 32
#error SPRITE_WINDOW + head + pizza must be <= 32 (hardware limit: sprites per scanline)
#endif
/* What decides whether the snake *dies* is deliberately independent of how many
   sprites we can afford to draw: self-collision by sample position covers this
   many segments, and the pixel test against the baked tail only applies past this length.
   (This is what SPRITE_WINDOW used to mean, so collisions behave as they always did.) */
#define COLLIDE_WINDOW 55
#if SPRITE_WINDOW > COLLIDE_WINDOW
#error SPRITE_WINDOW must not exceed COLLIDE_WINDOW
#endif
#define SEG_SPACING  2
/* The tail is erased by un-stamping its oldest dot (see "Tail canvas" below), which
   re-reads that dot's position from the trail ring buffer. A sample is overwritten
   TRAILSIZE frames after it was recorded, so the longest snake the buffer can still
   describe is TRAILSIZE/SEG_SPACING segments; MAX_LEN keeps a small margin under that.
   (Before this, curLen could grow past the buffer and the eraser then read positions
   that had already been overwritten.) 504 segments is far beyond what fits on screen
   without the snake running into itself anyway. */
#define MAX_LEN      ((TRAILSIZE / SEG_SPACING) - 8)
/* Body sprites sit *behind* the BG layer, so baking starts SPRITE_OVERLAP segments before
   the sprites run out: the baked dots (drawn from the identical shape) simply cover the
   sprites beneath them, which keeps the hand-off clear of the moving end of the sprite
   chain. (4 segments = 8 samples = ~13px.) */
#define SPRITE_OVERLAP  4
#define BAKE_SAMPLES    ((SPRITE_WINDOW - SPRITE_OVERLAP) * SEG_SPACING)    /* trail samples this old are baked into the BG */
#define SEG_SHIFT    1     /* log2(SEG_SPACING): drawSprites() uses shifts, not divide/multiply */
#if (1 << SEG_SHIFT) != SEG_SPACING
#error SEG_SHIFT must equal log2(SEG_SPACING)
#endif
#define TURN_RATE    5
#define MOVESPEED    210   /* desired sub-pixel speed per frame, Q7 (128=1px), ~1.6px/frame */
#define SELF_SKIP    8
#define SELF_R       3     /* pixel radius, cheap abs-based check (thin body) */
#define PIZZA_R      6
#define OAM_PIZZA    0
#define OAM_HEAD     4
#define OAM_BODY0    8

#define ST_TITLE     0
#define ST_PLAYING   1
#define ST_GAMEOVER  2
#define ST_ENTERNAME 3
#define ST_HISCORES  4

#define NAME_LEN     8
#define NUM_SCORES   20
#define SCORE_DIGITS 5     /* score is a u16 (max 65535), so 5 digits is exact */
#define NO_RANK      0xFF  /* "this game didn't make the table" */

/* Name entry: each position cycles through A-Z, 0-9 and space. */
#define NAME_CHAR_COUNT 37
#define NAME_ENTRY_COL  ((32 - NAME_LEN) / 2)
#define REPEAT_DELAY    20 /* frames Up/Down must be held before auto-repeat starts */
#define REPEAT_RATE     4  /* frames between repeats once it has started */

/* High score table screen layout (tile columns/rows). */
#define HS_COL_RANK   6
#define HS_COL_NAME   10
#define HS_COL_SCORE  20
#define HS_ROW_FIRST  5

/* Battery-backed SRAM save format (see saveHighScores()):
     [0..3]  magic "PZSN"
     [4]     NAME_LEN        } so changing either constant invalidates old
     [5]     NUM_SCORES      } saves instead of misreading them
     [6..]   NUM_SCORES x { NAME_LEN name bytes, score lo, score hi }
     last 2  checksum over everything before it (lo, hi) */
#define SAVE_HDR_LEN    6
#define SAVE_ENTRY_LEN  (NAME_LEN + 2)
#define SAVE_BODY_LEN   (SAVE_HDR_LEN + NUM_SCORES * SAVE_ENTRY_LEN)
#define SAVE_SIZE       (SAVE_BODY_LEN + 2)

//---------------------------------------------------------------------------------
u16 blockmap[0x400];

s16 trailX[TRAILSIZE];
s16 trailY[TRAILSIZE];
u16 headIdx;

u8 angle;
s32 posX, posY; /* Q7 fixed point (scaled by 128) */
u16 curLen;
u16 score;
u16 pizzaX, pizzaY;
u16 pizzaValue;
u8 pizzaBig;
u8 gameState;
u16 randSeed;
u16 pad0;
u8 hudDirty;
u8 hudFull;               /* 1 = labels + hi score must be (re)drawn too, not just the score */
u16 oamHighWater;
u16 snakeAge;
u16 nextBakeSample;
u16 nextEraseSample;
u8 snakeYoung;            /* 1 until snakeAge reaches BAKE_SAMPLES (see stepSnake) */

/* The tail canvas itself (counters, dirty queue, upload buffers) is declared with its
   code in the "Tail canvas" section below. */

char hiName[NAME_LEN + 1];
u8 nameLetters[NAME_LEN];
u8 nameCursor;

u16 hsScore[NUM_SCORES];
char hsName[NUM_SCORES][NAME_LEN + 1];
u8 newRank;               /* table slot of the score just entered, or NO_RANK */
u8 saveBuf[SAVE_SIZE];    /* staging buffer for SRAM reads/writes */

//---------------------------------------------------------------------------------
u16 nextRand(void)
{
    /* simple LCG, good enough for pizza placement */
    randSeed = randSeed * 25173 + 13849;
    return randSeed;
}

//---------------------------------------------------------------------------------
void clearMapRegion(u16 startCell, u16 count, u16 tile)
{
    u16 i;
    for (i = 0; i < count; i++)
        blockmap[startCell + i] = tile;
}

//---------------------------------------------------------------------------------
void writeText(u16 col, u16 row, const char *st)
{
    u16 p = row * 32 + col;
    u8 c;
    while ((c = *st))
    {
        blockmap[p] = asciiTile[c & 0x7F];
        p++;
        st++;
    }
}

//---------------------------------------------------------------------------------
u16 div10(u16 n)
{
    /* n / 10 without a divide (shift-and-add, Hacker's Delight). Exhaustively
       checked equal to n / 10 for all 65536 u16 values. The 65816 has no
       divide instruction, so a real u16 / and % are slow software routines;
       formatting the HUD used to do 20 of them in the one frame a pizza was
       eaten, which on a long snake was enough to blow the frame budget. */
    u16 q = (n >> 1) + (n >> 2);
    u16 r;
    q += q >> 4;
    q += q >> 8;
    q >>= 3;
    r = n - ((q << 3) + (q << 1));   /* q * 10 */
    return q + ((r + 6) >> 4);
}

//---------------------------------------------------------------------------------
void writeNum(u16 col, u16 row, u16 num, u8 len)
{
    u16 p = row * 32 + col + len - 1;
    u16 q;
    u8 i;
    for (i = 0; i < len; i++)
    {
        q = div10(num);
        blockmap[p] = asciiTile[(u8)(num - ((q << 3) + (q << 1))) + '0'];
        num = q;
        p--;
    }
}

//---------------------------------------------------------------------------------
void writeRepeat(u16 col, u16 row, char ch, u8 count)
{
    u16 p = row * 32 + col;
    u8 i;
    for (i = 0; i < count; i++)
        blockmap[p + i] = asciiTile[(u8)ch & 0x7F];
}

//---------------------------------------------------------------------------------
void waitReleased(u16 mask)
{
    /* Block until none of the buttons in `mask` is held. Every screen that
       reacts to START/A/B calls this before it starts listening: the button
       press that brought the player here (e.g. START confirming a name) is
       usually still physically down for a few frames, and without this it
       would instantly dismiss the *next* screen too -- the high score table
       would flash past unseen. Only the buttons the screen itself uses are
       waited on, so holding a d-pad direction from the moment of death
       doesn't stall anything. */
    while (padsCurrent(0) & mask)
        waitVBlank();
}

//---------------------------------------------------------------------------------
void drawWalls(void)
{
    u8 x, y;
    clearMapRegion(0, 0x400, TILE_BLANK);
    for (x = 0; x < 32; x++)
        blockmap[2 * 32 + x] = TILE_WALL;
    for (x = 0; x < 32; x++)
        blockmap[27 * 32 + x] = TILE_WALL;
    for (y = 3; y < 27; y++)
    {
        blockmap[y * 32 + 0] = TILE_WALL;
        blockmap[y * 32 + 31] = TILE_WALL;
    }
}

//---------------------------------------------------------------------------------
void hideAllSprites(void)
{
    u16 i;
    for (i = 0; i < 128; i++)
        oamSetVisible(i * 4, OBJ_HIDE);
    oamHighWater = 0;
}

//---------------------------------------------------------------------------------
void spawnPizza(void)
{
    u16 r;
    pizzaX = SPAWN_LEFT + 8 + (nextRand() % (SPAWN_RIGHT - SPAWN_LEFT - 16));
    pizzaY = SPAWN_TOP + 8 + (nextRand() % (SPAWN_BOTTOM - SPAWN_TOP - 16));

    r = nextRand() % 10;
    if (r < 3)
    {
        /* big pizza: rarer, worth more, grows more */
        pizzaBig = 1;
        pizzaValue = 20 + (nextRand() % 20); /* 20-39 */
        oamSetEx(OAM_PIZZA, OBJ_LARGE, OBJ_SHOW);
        oamSet(OAM_PIZZA, pizzaX - 8, pizzaY - 8, 2, 0, 0, SPR_PIZZA_BIG, 0);
    }
    else
    {
        pizzaBig = 0;
        pizzaValue = 5 + (nextRand() % 10); /* 5-14 */
        oamSetEx(OAM_PIZZA, OBJ_SMALL, OBJ_SHOW);
        oamSet(OAM_PIZZA, pizzaX - 4, pizzaY - 4, 2, 0, 0, SPR_PIZZA_SMALL, 0);
    }
}

//---------------------------------------------------------------------------------
// Tail canvas
//
// Everything older than the sprite window is drawn into the background layer. Rather than
// choosing from a library of pre-drawn connector tiles (which can only approximate a path
// -- it can't encode sub-pixel position or direction, so the baked tail came out thin,
// lumpy and misaligned next to the sprite part), the play area is a PIXEL CANVAS: each of
// its 30x24 cells owns a tile slot in VRAM (TILE_CANVAS_BASE + cell), the tilemap points
// at those slots permanently for the whole game, and the tail is drawn into them by
// stamping the very same 5x5 dot the body sprites use (generated from the one shape
// definition in gen_sprites.py), at every trail sample. The baked tail is thus the union
// of exactly the dots the sprites would have drawn: same width, same edge, same path,
// pixel for pixel, and nothing to line up at tile seams.
//
// Erasing the tail end is the same operation in reverse: un-stamp the oldest dot. Dots
// overlap each other (and, rarely, other coils of the body), so a pixel can't just be
// cleared when one dot leaves -- it must stay lit while any other dot still covers it.
// Each pixel therefore keeps a count of the dots covering it (max seen in practice: 4;
// the counters are 4 bits wide). The counting, and the matching VRAM-format tile data
// (palette index 5 wherever the count is non-zero), live in WRAM bank $7F and are
// maintained by canvasStampDot() in canvas.inc -- hand-written assembly, because the C
// compiler's version was slower than a whole frame. See canvas.inc for the layout.
//
// Changed cells are queued (each at most once) and DMA'd to VRAM straight from the
// canvasTile array, a few per frame, so the CPU/DMA cost doesn't depend on how long the
// snake is.
#define CANVAS_COL0    1     /* tilemap column of the canvas' left cell: x = 8..247 ... */
#define CANVAS_COLS    30    /* ...the interior inside the wall ring (see drawWalls()) */
#define CANVAS_ROW0    3     /* y = 24..215 */
#define CANVAS_ROWS    24
#define CANVAS_CELLS   (CANVAS_COLS * CANVAS_ROWS)
#define CANVAS_BYTES   (CANVAS_CELLS * 32)
#if CANVAS_BYTES != 23040
#error CANVAS_BYTES must match the .define in canvas.inc
#endif
#define BG_TILE_VRAM   0x1000   /* VRAM word address of the BG tile graphics (bgSetGfxPtr) */
#define DIRTYQ_SIZE    1024     /* power of two (canvas.inc hardcodes the mask), >= CANVAS_CELLS */
#define DIRTYQ_MASK    (DIRTYQ_SIZE - 1)
#define UPLOADS_PER_FRAME 16    /* canvas tiles sent to VRAM per frame (32 bytes each); a frame
                                   normally dirties ~6-8, any backlog just drains over a few frames */

/* All of the canvas tiles must sit below the sprite graphics at VRAM word 0x4000
   (16 words per 4bpp tile). */
#if (TILE_CANVAS_BASE + CANVAS_CELLS) * 16 + BG_TILE_VRAM > 0x4000
#error The canvas tiles no longer fit in VRAM below the sprite graphics
#endif
#if DIRTYQ_SIZE != 1024 || CANVAS_CELLS > DIRTYQ_SIZE
#error DIRTYQ_SIZE must be 1024 (see canvas.inc) and at least CANVAS_CELLS
#endif

static const u8 pixelBit[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
static const u8 zeroByte = 0;   /* in ROM: fixed-address source for the VRAM-clearing DMA */

extern u8 canvasTile;           /* canvas.inc: CANVAS_BYTES of VRAM-format tile data, bank $7F */
#define CANVAS_TILES ((u8 *)&canvasTile)
extern void canvasStampDot(void);    /* canvas.inc */
extern void clearCanvasWram(void);   /* canvas.inc: DMA-clears the counters and the tile data */

u16 stampX, stampY;             /* arguments of canvasStampDot() */
u8  stampAdd;
u8  cellQueued[CANVAS_CELLS];   /* canvasStampDot() queues each changed cell once... */
u16 dirtyQ[DIRTYQ_SIZE];        /* ...in this ring... */
u16 dirtyHead, dirtyTail;       /* ...(free-running indices, & DIRTYQ_MASK) */

//---------------------------------------------------------------------------------
static void stampDot(u16 x, u16 y, u8 add)
{
    /* Add (add = 1) or remove (add = 0) one body dot centred on screen pixel (x,y). */
    stampX = x;
    stampY = y;
    stampAdd = add;
    canvasStampDot();
}

//---------------------------------------------------------------------------------
static u8 canvasHit(u16 x, u16 y)
{
    /* Is the canvas pixel under screen position (x,y) part of the baked tail? */
    u16 row = (y >> 3) - CANVAS_ROW0;
    u16 col = (x >> 3) - CANVAS_COL0;
    if (row >= CANVAS_ROWS || col >= CANVAS_COLS)   /* unsigned: also catches "above" / "left of" */
        return 0;
    return (CANVAS_TILES[((((row << 5) - (row << 1)) + col) << 5) + ((y & 7) << 1)] & pixelBit[x & 7]) != 0;
}

//---------------------------------------------------------------------------------
static void flushUploads(void)
{
    /* Inside vblank: DMA the changed canvas tiles to VRAM. VRAM writes are only valid
       during vblank, so stop (leaving the rest queued for next frame) if it has ended --
       this can only matter if a frame dirtied far more cells than usual. */
    u8 n = 0;
    while (n < UPLOADS_PER_FRAME && dirtyHead != dirtyTail && (REG_HVBJOY & 0x80))
    {
        u16 cell = dirtyQ[dirtyHead & DIRTYQ_MASK];
        dirtyHead++;
        cellQueued[cell] = 0;
        dmaCopyVram(CANVAS_TILES + (cell << 5), BG_TILE_VRAM + ((TILE_CANVAS_BASE + cell) << 4), 32);
        n++;
    }
}

//---------------------------------------------------------------------------------
static void resetCanvas(void)
{
    /* New game: empty counters, empty VRAM tiles, and the tilemap pointing every
       playfield cell at its own canvas tile. The screen must already be force-blanked
       (VRAM is only writable then), and drawWalls() must already have run. */
    u16 r, c, t;
    clearCanvasWram();
    memset(cellQueued, 0, CANVAS_CELLS);
    dirtyHead = 0;
    dirtyTail = 0;
    dmaFillVram((u8 *)&zeroByte, BG_TILE_VRAM + (TILE_CANVAS_BASE << 4), CANVAS_BYTES);
    t = TILE_CANVAS_BASE;
    for (r = 0; r < CANVAS_ROWS; r++)
        for (c = 0; c < CANVAS_COLS; c++)
            blockmap[(CANVAS_ROW0 + r) * 32 + CANVAS_COL0 + c] = t++;
}

//---------------------------------------------------------------------------------
void resetGame(void)
{
    u16 i;
    posX = ((s32)128) << 7;
    posY = ((s32)112) << 7;
    angle = 0;
    curLen = 10;
    score = 0;
    headIdx = 0;
    hudDirty = 1;
    hudFull = 1;
    snakeAge = 0;
    nextBakeSample = 0;
    nextEraseSample = 0;
    snakeYoung = 1;
    for (i = 0; i < TRAILSIZE; i++)
    {
        trailX[i] = 128;
        trailY[i] = 112;
    }
    /* VRAM can only be written while the screen is force-blanked or in vblank, and
       clearing the canvas tiles is ~23 KB, more than one vblank can take -- so blank
       the screen here; the caller switches it back on once the new tilemap is sent. */
    setScreenOff();
    drawWalls();
    resetCanvas();
    hideAllSprites();
    oamHighWater = 2; /* oam #0 (pizza) and #1 (head) are managed separately below */
    oamSetEx(OAM_HEAD, OBJ_SMALL, OBJ_SHOW);
    oamSet(OAM_HEAD, (u16)(posX >> 7) - 4, (u16)(posY >> 7) - 4, 3, 0, 0, SPR_HEAD, 0);
    spawnPizza();
}

//---------------------------------------------------------------------------------
void drawHud(void)
{
    /* "SCORE nnnnn  HI NAMENAME nnnnn" -- fills cols 1..30 of row 0.
       Only the score value changes during a game, so the labels and the
       high score are drawn just once (hudFull, set by resetGame()); a pizza
       then costs one number instead of the whole line. Text/number drawing
       is slow on this CPU and this runs in the same frame as the pizza
       respawn, so it matters. */
    if (hudFull)
    {
        writeText(1, 0, "SCORE");
        writeText(14, 0, "HI");
        if (hsScore[0])
            writeText(17, 0, hsName[0]);
        writeNum(26, 0, hsScore[0], SCORE_DIGITS);
        hudFull = 0;
    }
    writeNum(7, 0, score, SCORE_DIGITS);
}

//---------------------------------------------------------------------------------
void drawSprites(void)
{
    u16 i;
    u16 hx, hy;
    u16 activeCount;
    u16 bodyCount;
    u16 tIdx;
    u8 *op;

    /* head: position only -- tile/attrs were set once in resetGame().
       Written directly into the OAM shadow buffer (auto-DMA'd at vblank by
       the library) instead of going through oamSetXY -- avoids a function
       call per sprite per frame, which matters once dozens of segments are
       being updated every single frame. */
    hx = (u16)(posX >> 7);
    hy = (u16)(posY >> 7);
    oamMemory[OAM_HEAD + 0] = (u8)(hx - 4);
    oamMemory[OAM_HEAD + 1] = (u8)(hy - 4);

    /* only the most recent SPRITE_WINDOW segments are rendered as sprites;
       older segments get permanently baked onto the BG layer in stepSnake(),
       which is how the snake can grow far past the 128-sprite hardware limit */
    bodyCount = curLen;
    if (bodyCount > SPRITE_WINDOW)
        bodyCount = SPRITE_WINDOW;

    /* newly active body slots: set tile/attr once (never changes again --
       a single fixed shape, so there's nothing to update per frame beyond
       position, and no risk of the flicker a per-frame-changing shape
       would cause) */
    activeCount = 2 + bodyCount;
    if (activeCount > oamHighWater)
    {
        for (i = oamHighWater; i < activeCount; i++)
        {
            oamSetEx(i * 4, OBJ_SMALL, OBJ_SHOW);
            oamMemory[i * 4 + 2] = SPR_BODY;
            oamMemory[i * 4 + 3] = OAM_ATTR(1, 0, 0, SPR_BODY, 0);
        }
        oamHighWater = activeCount;
    }

    /* every active body segment: cheap position-only update each frame.
       Walks the trail backwards from the head one SEG_SPACING at a time
       (rather than recomputing each index from scratch). */
    tIdx = (headIdx - SEG_SPACING) & TRAIL_MASK;
    op = &oamMemory[OAM_BODY0];
    while (bodyCount)
    {
        /* One contiguous run of the ring buffer: from tIdx back toward index 0
           in steps of SEG_SPACING. Pointer walking is much cheaper than
           re-indexing the arrays every iteration on this compiler. */
        u16 run = (tIdx >> SEG_SHIFT) + 1;   /* entries tIdx, tIdx-2, ... that stay >= 0 */
        s16 *px, *py;
        if (run > bodyCount)
            run = bodyCount;
        px = &trailX[tIdx];
        py = &trailY[tIdx];
        bodyCount -= run;
        tIdx = (tIdx - (run << SEG_SHIFT)) & TRAIL_MASK;   /* wraps to the top of the buffer */
        while (run--)
        {
            op[0] = (u8)(*px - 4);
            op[1] = (u8)(*py - 4);
            op += 4;
            px -= SEG_SPACING;
            py -= SEG_SPACING;
        }
    }
}

//---------------------------------------------------------------------------------
u8 stepSnake(void)
{
    s32 dx, dy;
    u16 hx, hy;
    u16 i;

    /* left/right reversed per player preference */
    if (pad0 & KEY_LEFT)
        angle += TURN_RATE;
    if (pad0 & KEY_RIGHT)
        angle -= TURN_RATE;

    /* 16-bit multiply on purpose: |table| <= 128 and MOVESPEED = 210 give at
       most 26880, which fits in 16 bits -- and a 32-bit multiply is a slow
       software routine on the 65816 (2 of them ran every single frame) */
    dx = (cosTable[angle] * MOVESPEED) >> 7;
    dy = -((sinTable[angle] * MOVESPEED) >> 7);
    posX += dx;
    posY += dy;

    hx = (u16)(posX >> 7);
    hy = (u16)(posY >> 7);

    headIdx = (headIdx + 1) & TRAIL_MASK;
    trailX[headIdx] = hx;
    trailY[headIdx] = hy;
    snakeAge++;

    if (hx < ARENA_LEFT || hx > ARENA_RIGHT || hy < ARENA_TOP || hy > ARENA_BOTTOM)
        return 1; /* wall death */

    /* Bake segments that have aged out of the sprite-rendering window into the
       background canvas (stamp their dot), and erase the oldest baked segment (un-stamp
       it) once it falls outside the snake's actual current length. Since the snake only
       grows via eating (not via distance travelled), the *total* body length must stay
       fixed at curLen -- without erasing, the baked tail would just accumulate the
       snake's entire lifetime path forever, which is wrong.

       Every sample is stamped exactly once and un-stamped exactly once, each time from
       the same trail position, so the counters always describe exactly the samples in
       [nextEraseSample, nextBakeSample). Both cursors are kept pinned to "now" whenever
       they're not actively needed, rather than left frozen, so there is never a backlog
       to catch up on in one frame. They are free-running u16 sample numbers compared
       modulo 65536 (the gap is always tiny), so they stay correct when snakeAge wraps. */
    {
        u16 bakeTarget;
        u16 eraseTarget = snakeAge - curLen * SEG_SPACING;

        /* Until the snake is BAKE_SAMPLES old there are no samples that old: hold the
           cursor at sample 0 instead of letting it run into "negative" ages, which
           index trail slots that still hold the spawn-point fill and would stamp the
           same dot over and over (a 4-bit counter wraps at 16). A flag rather than a
           comparison, because snakeAge itself wraps after ~18 minutes of play. */
        if (snakeYoung && snakeAge >= BAKE_SAMPLES)
            snakeYoung = 0;
        bakeTarget = snakeYoung ? 0 : (u16)(snakeAge - BAKE_SAMPLES);

        if (curLen <= SPRITE_WINDOW)
        {
            nextBakeSample = bakeTarget;
            nextEraseSample = bakeTarget;
        }
        else
        {
            u8 guard = 12; /* defensive cap: never more than a handful of ops/frame */
            while ((s16)(bakeTarget - nextBakeSample) >= 0 && guard--)
            {
                u16 bIdx = nextBakeSample & TRAIL_MASK;
                stampDot((u16)trailX[bIdx], (u16)trailY[bIdx], 1);
                nextBakeSample++;
            }

            guard = 12;
            while ((s16)(eraseTarget - nextEraseSample) >= 0 && guard--)
            {
                u16 eIdx = nextEraseSample & TRAIL_MASK;
                stampDot((u16)trailX[eIdx], (u16)trailY[eIdx], 0);
                nextEraseSample++;
            }
        }
    }

    /* cheap abs-based self collision against the most recent COLLIDE_WINDOW
       segments, sampling every other segment. Never look further back than the snake
       has actually been alive for, or we'd read the trail buffer's initial
       spawn-point stub data and get a false "collision" near the start of
       a life. */
    {
        u16 maxCheck = curLen;
        u16 ageLimit = snakeAge / SEG_SPACING;
        if (ageLimit < maxCheck)
            maxCheck = ageLimit;
        if (maxCheck > COLLIDE_WINDOW)
            maxCheck = COLLIDE_WINDOW;

        u16 tIdx = (headIdx - SELF_SKIP * SEG_SPACING) & TRAIL_MASK;
        for (i = SELF_SKIP; i < maxCheck; i += 2)
        {
            /* |dx| < SELF_R && |dy| < SELF_R, written as one unsigned compare per
               axis: (u16)(d + SELF_R - 1) < 2*SELF_R - 1 is true exactly when
               -SELF_R < d < SELF_R. Same result as abs(), without the branches. */
            u16 adx = (u16)(hx - trailX[tIdx] + (SELF_R - 1));
            u16 ady = (u16)(hy - trailY[tIdx] + (SELF_R - 1));
            if (adx < (2 * SELF_R - 1) && ady < (2 * SELF_R - 1))
                return 1; /* self collision */
            tIdx = (tIdx - 2 * SEG_SPACING) & TRAIL_MASK; /* next sample: i += 2 */
        }
    }

    /* collision against the older, baked tail: one pixel lookup. The head's centre
       dies if it is on a pixel of the baked body -- the same rule the sample check
       above applies to the sprite part (the centre inside a body dot's footprint),
       so what you see is what kills you. Applies from COLLIDE_WINDOW on; younger
       segments are covered by the sample-position check. */
    if (curLen > COLLIDE_WINDOW && canvasHit(hx, hy))
        return 1;

    {
        s16 adx = (s16)hx - (s16)pizzaX;
        s16 ady = (s16)hy - (s16)pizzaY;
        u8 r = pizzaBig ? (PIZZA_R + 4) : PIZZA_R;
        if (adx < 0) adx = -adx;
        if (ady < 0) ady = -ady;
        if (adx < r && ady < r)
        {
            score += pizzaValue;
            curLen += pizzaValue / 3;
            if (curLen > MAX_LEN)
                curLen = MAX_LEN;
            hudDirty = 1;
            spawnPizza();
        }
    }

    return 0;
}

//---------------------------------------------------------------------------------
// High score persistence (battery-backed SRAM, 2 KB at $70:0000 on LoROM)
//---------------------------------------------------------------------------------
u16 saveChecksum(u16 len)
{
    /* Position-sensitive rotate-and-add over saveBuf[0..len). Not
       cryptographic -- just enough to notice a blank/garbage/foreign SRAM. */
    u16 sum = 0x5A5A;
    u16 i;
    for (i = 0; i < len; i++)
    {
        sum = (u16)((sum << 1) | (sum >> 15));
        sum += saveBuf[i];
    }
    return sum;
}

//---------------------------------------------------------------------------------
void clearHighScores(void)
{
    u8 i, j;
    for (i = 0; i < NUM_SCORES; i++)
    {
        hsScore[i] = 0;
        for (j = 0; j < NAME_LEN; j++)
            hsName[i][j] = ' ';
        hsName[i][NAME_LEN] = 0;
    }
}

//---------------------------------------------------------------------------------
void saveHighScores(void)
{
    u16 p, sum;
    u8 i, j;

    saveBuf[0] = 'P';
    saveBuf[1] = 'Z';
    saveBuf[2] = 'S';
    saveBuf[3] = 'N';
    saveBuf[4] = NAME_LEN;
    saveBuf[5] = NUM_SCORES;

    p = SAVE_HDR_LEN;
    for (i = 0; i < NUM_SCORES; i++)
    {
        for (j = 0; j < NAME_LEN; j++)
        {
            saveBuf[p] = (u8)hsName[i][j];
            p++;
        }
        saveBuf[p] = (u8)(hsScore[i] & 0xFF);
        saveBuf[p + 1] = (u8)(hsScore[i] >> 8);
        p += 2;
    }

    sum = saveChecksum(SAVE_BODY_LEN);
    saveBuf[SAVE_BODY_LEN] = (u8)(sum & 0xFF);
    saveBuf[SAVE_BODY_LEN + 1] = (u8)(sum >> 8);

    consoleCopySram(saveBuf, SAVE_SIZE);
}

//---------------------------------------------------------------------------------
u8 saveBufIsValid(void)
{
    u16 sum;
    if (saveBuf[0] != 'P' || saveBuf[1] != 'Z' || saveBuf[2] != 'S' || saveBuf[3] != 'N')
        return 0;
    if (saveBuf[4] != NAME_LEN || saveBuf[5] != NUM_SCORES)
        return 0;
    sum = saveChecksum(SAVE_BODY_LEN);
    if (saveBuf[SAVE_BODY_LEN] != (u8)(sum & 0xFF))
        return 0;
    if (saveBuf[SAVE_BODY_LEN + 1] != (u8)(sum >> 8))
        return 0;
    return 1;
}

//---------------------------------------------------------------------------------
void loadHighScores(void)
{
    /* Called once at power-on. Blank, corrupt or foreign SRAM (first boot,
       an emulator that hands back zeroes/0xFF, a flashcart without SRAM
       mapped) falls back to an empty table, which is then written back so
       the next boot finds a valid save. The game still works with no SRAM
       present -- scores just last until power-off. */
    u16 p;
    u8 i, j;

    consoleLoadSram(saveBuf, SAVE_SIZE);
    if (!saveBufIsValid())
    {
        clearHighScores();
        saveHighScores();
        return;
    }

    p = SAVE_HDR_LEN;
    for (i = 0; i < NUM_SCORES; i++)
    {
        for (j = 0; j < NAME_LEN; j++)
        {
            hsName[i][j] = (char)saveBuf[p];
            p++;
        }
        hsName[i][NAME_LEN] = 0;
        hsScore[i] = (u16)saveBuf[p] | ((u16)saveBuf[p + 1] << 8);
        p += 2;
    }
}

//---------------------------------------------------------------------------------
u8 qualifiesForHighScore(void)
{
    return (score > 0 && score > hsScore[NUM_SCORES - 1]);
}

//---------------------------------------------------------------------------------
u8 insertHighScore(u16 s, const char *name)
{
    /* Returns the slot the score landed in, or NO_RANK if it didn't make
       the table. Equal scores rank below existing ones (first to reach a
       score keeps the higher spot). */
    s8 slot, i;
    u8 j;
    slot = -1;
    for (i = 0; i < NUM_SCORES; i++)
    {
        if (s > hsScore[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return NO_RANK;
    for (i = NUM_SCORES - 1; i > slot; i--)
    {
        hsScore[i] = hsScore[i - 1];
        for (j = 0; j <= NAME_LEN; j++)
            hsName[i][j] = hsName[i - 1][j];
    }
    hsScore[slot] = s;
    for (j = 0; j < NAME_LEN; j++)
        hsName[slot][j] = name[j];
    hsName[slot][NAME_LEN] = 0;
    return (u8)slot;
}

//---------------------------------------------------------------------------------
char nameCharFromIndex(u8 idx)
{
    if (idx < 26)
        return (char)('A' + idx);
    if (idx < 36)
        return (char)('0' + (idx - 26));
    return ' ';
}

//---------------------------------------------------------------------------------
void drawNameRow(u16 row, u8 blinkOn)
{
    /* Spaces are drawn as '_' placeholders so all NAME_LEN slots are visible
       while typing (an actual space is invisible), and the cursor blinks
       between its glyph and blank -- which also works on a placeholder. */
    u8 i;
    char c;
    for (i = 0; i < NAME_LEN; i++)
    {
        c = nameCharFromIndex(nameLetters[i]);
        if (c == ' ')
            c = '_';
        if (i == nameCursor && !blinkOn)
            blockmap[row * 32 + NAME_ENTRY_COL + i] = TILE_BLANK;
        else
            blockmap[row * 32 + NAME_ENTRY_COL + i] = asciiTile[(u8)c];
    }
}

//---------------------------------------------------------------------------------
void nameStepChar(u8 up)
{
    if (up)
        nameLetters[nameCursor] = (nameLetters[nameCursor] + NAME_CHAR_COUNT - 1) % NAME_CHAR_COUNT;
    else
        nameLetters[nameCursor] = (nameLetters[nameCursor] + 1) % NAME_CHAR_COUNT;
}

//---------------------------------------------------------------------------------
void nameEntryScreen(void)
{
    u16 padOld;
    u16 frameCount = 0;
    u16 pressed;
    u16 hold = 0;
    u8 i;

    /* Start as "A" followed by spaces: a short name needs no cleanup, and
       with 8 positions that saves a lot of button-mashing. */
    nameLetters[0] = 0;
    for (i = 1; i < NAME_LEN; i++)
        nameLetters[i] = NAME_CHAR_COUNT - 1;
    nameCursor = 0;

    drawWalls();
    writeText(9, 8, "NEW HIGH SCORE");
    writeText(10, 10, "SCORE");
    writeNum(16, 10, score, SCORE_DIGITS);
    writeText(8, 13, "ENTER YOUR NAME");
    writeText(8, 19, "LEFT RIGHT MOVE");
    writeText(9, 21, "UP DOWN CHANGE");
    writeText(9, 23, "START CONFIRM");

    /* Treat anything already held when the game ended (typically the
       d-pad turn button) as old, not as a fresh press. */
    padOld = padsCurrent(0);

    for (;;)
    {
        pad0 = padsCurrent(0);
        pressed = pad0 & (u16)(~padOld);
        padOld = pad0;

        if (pressed & KEY_LEFT)
            nameCursor = (nameCursor == 0) ? 0 : nameCursor - 1;
        if (pressed & KEY_RIGHT)
            nameCursor = (nameCursor >= NAME_LEN - 1) ? nameCursor : nameCursor + 1;

        /* Up/Down: step on press, then auto-repeat while held (there are
           37 characters to cycle through, one press each would be tedious) */
        if (pressed & (KEY_UP | KEY_DOWN))
        {
            nameStepChar((pressed & KEY_UP) ? 1 : 0);
            hold = 0;
        }
        else if (pad0 & (KEY_UP | KEY_DOWN))
        {
            hold++;
            if (hold >= REPEAT_DELAY && ((hold - REPEAT_DELAY) % REPEAT_RATE) == 0)
                nameStepChar((pad0 & KEY_UP) ? 1 : 0);
        }
        else
        {
            hold = 0;
        }

        drawNameRow(15, (frameCount >> 4) & 1);

        waitVBlank();
        dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
        frameCount++;

        if (pressed & KEY_START)
            break;
    }

    for (i = 0; i < NAME_LEN; i++)
        hiName[i] = nameCharFromIndex(nameLetters[i]);
    hiName[NAME_LEN] = 0;

    newRank = insertHighScore(score, hiName);
    if (newRank != NO_RANK)
        saveHighScores();
}

//---------------------------------------------------------------------------------
void drawScoreRow(u8 i, u8 show)
{
    /* One table row: rank always, name + score only when `show` (so the
       freshly-entered row can blink without its rank number jumping). Empty
       slots show dashes. */
    u16 row = HS_ROW_FIRST + i;
    writeNum(HS_COL_RANK, row, i + 1, 2);
    if (!show)
    {
        clearMapRegion(row * 32 + HS_COL_NAME, HS_COL_SCORE + SCORE_DIGITS - HS_COL_NAME, TILE_BLANK);
        return;
    }
    if (hsScore[i])
    {
        writeText(HS_COL_NAME, row, hsName[i]);
        writeNum(HS_COL_SCORE, row, hsScore[i], SCORE_DIGITS);
    }
    else
    {
        writeRepeat(HS_COL_NAME, row, '-', NAME_LEN);
        writeRepeat(HS_COL_SCORE, row, '-', SCORE_DIGITS);
    }
}

//---------------------------------------------------------------------------------
void hiScoresScreen(void)
{
    u8 i;
    u8 blinkOn = 1;
    u8 rowDirty = 0;
    u16 frameCount = 0;

    drawWalls();
    writeText(10, 1, "HIGH SCORES");
    writeText(HS_COL_RANK, 3, "NO");
    writeText(HS_COL_NAME, 3, "NAME");
    writeText(HS_COL_SCORE, 3, "SCORE");
    for (i = 0; i < NUM_SCORES; i++)
        drawScoreRow(i, 1);
    writeText(10, 26, "PRESS START");
    waitVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START | KEY_A | KEY_B);
    for (;;)
    {
        pad0 = padsCurrent(0);

        /* blink the row the player just earned, so it's easy to spot in a
           table of 20 */
        if (newRank != NO_RANK)
        {
            u8 phase = (u8)((frameCount >> 4) & 1) ^ 1; /* start visible */
            if (phase != blinkOn)
            {
                blinkOn = phase;
                drawScoreRow(newRank, blinkOn);
                rowDirty = 1;
            }
        }

        waitVBlank();
        if (rowDirty)
        {
            u16 cell = (u16)(HS_ROW_FIRST + newRank) * 32;
            dmaCopyVram((u8 *)&blockmap[cell], cell, 64);
            rowDirty = 0;
        }
        frameCount++;

        if (pad0 & (KEY_START | KEY_B | KEY_A))
            break;
    }
    newRank = NO_RANK;
    gameState = ST_TITLE;
}

//---------------------------------------------------------------------------------
void titleScreen(void)
{
    drawWalls();
    writeText(10, 8, "PIZZA SNAKE");
    writeText(8, 12, "PRESS START");
    writeText(3, 14, "PRESS SELECT FOR SCORES");
    if (hsScore[0])
    {
        writeText(4, 18, "HI SCORE");
        writeText(13, 18, hsName[0]);
        writeNum(22, 18, hsScore[0], SCORE_DIGITS);
    }
    waitVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START | KEY_SELECT);
    for (;;)
    {
        pad0 = padsCurrent(0);
        waitVBlank();
        if (pad0 & KEY_START)
        {
            resetGame();            /* leaves the screen force-blanked */
            /* resetGame() only rebuilt the tilemap in RAM; push it to VRAM so
               the title text doesn't stay painted on the playing field (the
               in-game loop only ever uploads the HUD row and canvas tiles) */
            waitVBlank();
            dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);
            setBrightness(15);
            setScreenOn();
            gameState = ST_PLAYING;
            return;
        }
        if (pad0 & KEY_SELECT)
        {
            gameState = ST_HISCORES;
            return;
        }
    }
}

//---------------------------------------------------------------------------------
void gameOverScreen(void)
{
    hideAllSprites();

    /* After every game the player ends up on the high score table:
       qualifying score -> name entry -> table (new row blinking),
       otherwise        -> game over / final score -> table. */
    if (qualifiesForHighScore())
    {
        nameEntryScreen();
        gameState = ST_HISCORES;
        return;
    }

    newRank = NO_RANK;
    drawWalls();
    writeText(11, 12, "GAME OVER");
    writeText(10, 15, "SCORE");
    writeNum(16, 15, score, SCORE_DIGITS);
    writeText(10, 18, "PRESS START");
    waitVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    waitReleased(KEY_START);
    for (;;)
    {
        pad0 = padsCurrent(0);
        waitVBlank();
        if (pad0 & KEY_START)
            break;
    }
    gameState = ST_HISCORES;
}

//---------------------------------------------------------------------------------
/* Full-screen splash using the Pizza Worm title art.
   Fades in, waits for Start, then fades out before the normal title. */
void splashScreen(void)
{
    /* Force the screen completely off / black while we load everything.
       This prevents any unfaded frame from appearing at ROM boot. */
    setScreenOff();
    setBrightness(0);

    dmaCopyVram(&gfxsplash, 0x1000, (&gfxsplash_end - &gfxsplash));
    dmaCopyVram(&mapsplash, 0x0000, (&mapsplash_end - &mapsplash));
    dmaCopyCGram((u8 *)&palsplash, 0, (&palsplash_end - &palsplash));

    bgSetGfxPtr(0, 0x1000);
    bgSetMapPtr(0, 0x0000, SC_32x32);
    bgSetScroll(0, 0, 0);          /* map row 0 on screen row 0 for the art */

    setMode(BG_MODE1, 0);
    bgSetDisable(1);
    bgSetDisable(2);
    hideAllSprites();

    /* Enable the screen while still at brightness 0.  setScreenOn() may
       internally touch INIDISP, so we re-assert brightness 0 immediately
       afterwards and wait a couple of VBlanks so the first visible frame
       is pure black.  Only then do we start the fade-in. */
    setScreenOn();
    setBrightness(0);
    waitVBlank();
    setBrightness(0);
    waitVBlank();
    setBrightness(0);

    setFadeEffectEx(FADE_IN, 12);

    waitReleased(KEY_START);
    for (;;)
    {
        pad0 = padsCurrent(0);
        waitVBlank();
        if (pad0 & KEY_START)
            break;
    }

    /* Fade back to black, then the caller reloads the game graphics */
    setFadeEffectEx(FADE_OUT, 12);
    /* Wait for the fade to finish so we don't tear when overwriting VRAM. */
    {
        u8 i;
        for (i = 0; i < 14; i++)
            waitVBlank();
    }
    setBrightness(0);
    setScreenOff();
}

//---------------------------------------------------------------------------------
int main(void)
{
    /* Start completely dark / blank so nothing flashes before the splash. */
    setScreenOff();
    setBrightness(0);
    WaitForVBlank();

    /* Initialize SPC700 sound engine (copies the Impulse Tracker driver) */
    spcBoot();

    /* Tell the driver where the soundbank lives in ROM */
    spcSetBank(&SOUNDBANK__);

    /* Load the pizza music module (takes a moment while data is transferred) */
    spcLoad(MOD_PIZZA_MUSIC);

    /* Start playing from the beginning; it will loop according to the module */
    spcPlay(0);

    randSeed = 0xACE1;
    newRank = NO_RANK;
    loadHighScores();
    gameState = ST_TITLE;

    /* Initial splash screen (fades in / out on Start) */
    splashScreen();

    /* Now load the real game graphics (overwrites the splash tiles/map) */
    oamInitGfxSet(&gfxsprites, (&gfxsprites_end - &gfxsprites),
                   &palsprites, (&palsprites_end - &palsprites),
                   0, 0x4000, OBJ_SIZE8_L16);

    dmaCopyVram(&gfxwalls, 0x1000, (&gfxwalls_end - &gfxwalls));
    dmaCopyCGram((u8 *)&palwalls, 0, (&palwalls_end - &palwalls));

    drawWalls();
    waitVBlank();
    dmaCopyVram((u8 *)blockmap, 0x0000, 0x800);

    bgSetGfxPtr(0, 0x1000);
    bgSetMapPtr(0, 0x0000, SC_32x32);

    setMode(BG_MODE1, 0);
    bgSetDisable(1);
    bgSetDisable(2);

    hideAllSprites();
    /* The SNES starts drawing one line *into* the background: with a vertical
       scroll of 0, tilemap pixel row 0 is never shown and everything sits 1px
       too high -- which clipped the top stroke off the HUD text on tile row 0.
       A scroll of -1 puts tilemap row N on screen line N, matching the pixel
       coordinates the sprites and tile lookups already use. */
    bgSetScroll(0, 0, 0xFFFF);

    /* Screen was left black by the splash fade-out.  Jump straight to full
       brightness with no second fade-in. */
    setBrightness(15);
    setScreenOn();

    for (;;)
    {
        switch (gameState)
        {
        case ST_TITLE:
            titleScreen();
            break;

        case ST_HISCORES:
            hiScoresScreen();
            break;

        case ST_PLAYING:
            pad0 = padsCurrent(0);
            if (stepSnake())
            {
                gameState = ST_GAMEOVER;
                break;
            }
            if (hudDirty)
                drawHud();
            drawSprites();
            waitVBlank();
            if (hudDirty)
            {
                /* HUD text only ever touches row 0 -- 32 cells, 64 bytes */
                dmaCopyVram((u8 *)blockmap, 0x0000, 64);
                hudDirty = 0;
            }
            flushUploads();         /* canvas tiles. The tilemap itself never changes during
                                       play: every cell permanently points at its own tile. */
            break;

        case ST_GAMEOVER:
            gameOverScreen();
            break;
        }
    }
    return 0;
}
