; =====================================================================================
; Tail canvas  (the design is described under "Tail canvas" in psnake.c)
;
; The baked tail is drawn into the background layer pixel by pixel by stamping the body dot
; at every trail sample, and erased by un-stamping the oldest dot.  Because dots overlap, each
; pixel keeps a COUNT of the dots covering it, kept as four bit-planes so that adding or
; removing a whole row of up to 5 pixels is a short ripple-carry over bytes.
;
; Both arrays below are laid out exactly like 4bpp SNES tile data, one 32-byte tile per
; playfield cell (720 cells), so ONE index X = cell*32 + 2*row addresses every byte of a pixel
; row in both of them:
;     canvasCnt  [X+0] [X+1] [X+16] [X+17]  = count bit-planes 0..3 of that row's 8 pixels
;     canvasTile [X+0] and [X+16]           = the VRAM tile: palette index 5 (%0101, the body
;                                             green) wherever the count is non-zero
; canvasTile is DMA'd to VRAM as-is, so there is no per-frame tile building at all.
;
; Why bank $7F and why assembly:
;  * The C compiler's .bss starts at $7E:2000 and pvsneslib's own RAM sections (console text
;    buffer, OAM shadow buffer, ...) are fixed at $7E:8000 and above -- only ~24 KB of $7E is
;    available to C globals, and the build does NOT complain when a bigger .bss runs over the
;    library's sections (it silently corrupts them).  Bank $7F is free.
;  * 816-tcc turns every access through a 24-bit pointer into ~10 instructions; written in C
;    this routine cost more than a whole frame.  Here it is a few thousand cycles.
; =====================================================================================
.define CANVAS_BYTES 23040              ; 720 cells * 32 bytes; psnake.c checks it matches

.RAMSECTION ".canvas7f" BANK $7F SLOT 3
canvasCnt  dsb CANVAS_BYTES             ; MUST stay directly before canvasTile (one DMA clears both)
canvasTile dsb CANVAS_BYTES
cvCol      dsw 1                        ; scratch for canvasStampDot
cvY        dsw 1
cvRows     dsw 1
cvBitsOff  dsw 1
cvBits     dsw 1
cvCell     dsw 1
cvR2       dsw 1
cvMask     dsw 1
.ENDS

.SECTION ".canvas_rodata" SUPERFREE
; tilemap row -> index of that row's first canvas cell ($FFFF = HUD or wall row: not drawn)
canvasRowCell:
    .dw $FFFF, $FFFF, $FFFF, 0, 30, 60, 90, 120
    .dw 150, 180, 210, 240, 270, 300, 330, 360
    .dw 390, 420, 450, 480, 510, 540, 570, 600
    .dw 630, 660, 690, $FFFF, $FFFF, $FFFF, $FFFF, $FFFF

.include "body_dot.inc"
.ENDS

; -------------------------------------------------------------------------------------
; clearCanvasWram(): zero both canvas arrays with one DMA (ROM zero byte -> WRAM port $2180,
; channel 1, fixed source address); a software loop over 46 KB would take several frames.
; Written here, not in C, because it needs exact 24-bit addresses, which the assembler/linker
; hand over directly (:label = bank); 816-tcc's 32-bit pointer arithmetic did not reliably
; produce the bank bit for $7F.
; -------------------------------------------------------------------------------------
.SECTION ".clearcanvas_text" SUPERFREE
clearCanvasWram:
    php
    rep #$20
    lda.w #canvasCnt
    sta.l $002181               ; WMADDL/M: low 16 bits of the destination
    sep #$20
    lda.b #:canvasCnt
    and.b #$01
    sta.l $002183               ; WMADDH: bit 0 selects bank $7E / $7F
    lda.b #$08
    sta.l $004310               ; DMAP1: CPU -> B bus, A-bus address fixed, 1 register
    lda.b #$80
    sta.l $004311               ; BBAD1: $2180 (WRAM data port)
    rep #$20
    lda.w #canvasZeroByte
    sta.l $004312               ; A1T1: source address
    sep #$20
    lda.b #:canvasZeroByte
    sta.l $004314               ; A1B1: source bank
    rep #$20
    lda.w #CANVAS_BYTES * 2
    sta.l $004315               ; DAS1: byte count (canvasCnt + canvasTile)
    sep #$20
    lda.b #$02
    sta.l $00420B               ; MDMAEN: go (channel 1)
    plp
    rtl
canvasZeroByte:
    .db 0
.ENDS

; -------------------------------------------------------------------------------------
; canvasStampDot(): add (stampAdd != 0) or remove (stampAdd == 0) the body dot centred on
; screen pixel (stampX, stampY).  Arguments are C globals; nothing is returned.
;
; Works in screen pixels: ax = x-2 is the dot's left pixel, and ax>>3 its tilemap column.  A
; dot is at most 5 wide, so each of its 5 rows touches one cell or spills into the next one to
; the right; bodyDotBits holds each row's mask already split that way (high byte = left cell,
; low byte = right cell).  Rows/columns outside the canvas (the 1px of dot that can overlap the
; wall outline at the arena edge) are clipped; the wall tile is opaque so the sprites never
; showed those pixels either.  Every cell touched is queued for upload (once).
; -------------------------------------------------------------------------------------
.SECTION ".canvasstamp_text" SUPERFREE
canvasStampDot:
    php
    rep #$30
    lda.l stampX
    sec
    sbc.w #2                    ; ax = left pixel of the dot
    pha
    and.w #7
    asl a
    sta.l cvBitsOff             ; byte offset of this x-offset within a bodyDotBits row
    pla
    lsr a
    lsr a
    lsr a
    sta.l cvCol                 ; col = ax >> 3
    lda.l stampY
    sec
    sbc.w #2
    sta.l cvY                   ; pixel row of the dot's top edge
    lda.w #5
    sta.l cvRows

@rowLoop:
    lda.l cvY
    lsr a
    lsr a
    lsr a
    asl a
    tax
    lda.l canvasRowCell,x       ; $FFFF = not a canvas row
    cmp.w #$FFFF
    bne @inCanvas
    jmp @nextRow
@inCanvas:
    clc
    adc.l cvCol
    dec a                       ; cell index of the LEFT cell = rowCell + col - 1
    sta.l cvCell
    lda.l cvBitsOff
    tax
    lda.l bodyDotBits,x
    sta.l cvBits
    lda.l cvY
    and.w #7
    asl a
    sta.l cvR2                  ; pixel row within the cell, x2 (4bpp rows are 2 bytes apart)

    lda.l cvCol                 ; left cell exists when 1 <= col <= 30
    dec a
    cmp.w #30
    bcs @right
    lda.l cvBits
    xba
    and.w #$00FF                ; high byte: the left cell's row mask
    sta.l cvMask
    lda.l cvCell
    jsr doRow
@right:
    lda.l cvBits
    and.w #$00FF                ; low byte: the part that spills into the right cell
    beq @nextRow
    sta.l cvMask
    lda.l cvCol                 ; right cell (col+1) exists when col <= 29
    cmp.w #30
    bcs @nextRow
    lda.l cvCell
    inc a
    jsr doRow
@nextRow:
    lda.l cvY
    inc a
    sta.l cvY
    lda.l cvBitsOff
    clc
    adc.w #16                   ; next row of bodyDotBits
    sta.l cvBitsOff
    lda.l cvRows
    dec a
    sta.l cvRows
    beq @done
    jmp @rowLoop
@done:
    plp
    rtl

; doRow: A = canvas cell index (16 bit); cvR2 = pixel row x2; cvMask = the pixels to change.
doRow:
    asl a
    asl a
    asl a
    asl a
    asl a                       ; cell * 32
    clc
    adc.l cvR2
    tax                         ; X = byte offset of this pixel row in both canvas arrays
    sep #$20                    ; 8-bit accumulator from here on
    lda.l stampAdd
    bne @add
    jmp rowSub
@add:
    ; falls into rowAdd

; rowAdd: add 1 to the count of every pixel set in cvMask.  Bit-sliced ripple carry, plane by
; plane: new = plane ^ carry, carry_out = plane & carry; stops as soon as the carry is empty.
rowAdd:
    lda.l cvMask
    pha
    and.l canvasCnt,x
    tay
    pla
    eor.l canvasCnt,x
    sta.l canvasCnt,x
    tya
    beq rowTail
    pha
    and.l canvasCnt+1,x
    tay
    pla
    eor.l canvasCnt+1,x
    sta.l canvasCnt+1,x
    tya
    beq rowTail
    pha
    and.l canvasCnt+16,x
    tay
    pla
    eor.l canvasCnt+16,x
    sta.l canvasCnt+16,x
    tya
    beq rowTail
    eor.l canvasCnt+17,x        ; last plane: the carry out is dropped (counts wrap at 16)
    sta.l canvasCnt+17,x

; rowTail: refresh the VRAM tile row (pixel set where any count plane is set) and queue the cell.
rowTail:
    lda.l canvasCnt,x
    ora.l canvasCnt+1,x
    ora.l canvasCnt+16,x
    ora.l canvasCnt+17,x
    sta.l canvasTile,x          ; plane 0
    sta.l canvasTile+16,x       ; plane 2   (palette index 5 = %0101)
    rep #$20
    txa
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    tax                         ; X = cell index
    sep #$20
    lda.l cellQueued,x
    bne @queued
    lda.b #1
    sta.l cellQueued,x
    rep #$20
    lda.l dirtyTail
    and.w #$03FF                ; DIRTYQ_MASK
    asl a
    tay
    txa                         ; A = cell index, to be stored...
    tyx                         ; ...at dirtyQ[tail & mask]
    sta.l dirtyQ,x
    lda.l dirtyTail
    inc a
    sta.l dirtyTail
    rts
@queued:
    rep #$20
    rts

; rowSub: subtract 1 from the count of every pixel set in cvMask: the same ripple, with a borrow
; (borrow_out = ~plane & borrow) instead of a carry.
rowSub:
    lda.l cvMask
    pha
    lda.l canvasCnt,x
    eor.b #$FF
    and 1,s
    tay
    pla
    eor.l canvasCnt,x
    sta.l canvasCnt,x
    tya
    beq @toTail
    pha
    lda.l canvasCnt+1,x
    eor.b #$FF
    and 1,s
    tay
    pla
    eor.l canvasCnt+1,x
    sta.l canvasCnt+1,x
    tya
    beq @toTail
    pha
    lda.l canvasCnt+16,x
    eor.b #$FF
    and 1,s
    tay
    pla
    eor.l canvasCnt+16,x
    sta.l canvasCnt+16,x
    tya
    beq @toTail
    eor.l canvasCnt+17,x
    sta.l canvasCnt+17,x
@toTail:
    jmp rowTail                 ; (the early exits above are too far back for a short branch)
.ENDS
