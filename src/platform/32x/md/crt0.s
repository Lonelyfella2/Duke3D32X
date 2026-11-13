| ---------------------------------------------------------------------------
| Duke Nukem 3D 32X - Mega Drive (68000) side.
|
| Based on Chilly Willy's 32X 68000 support code (roq32x src-md/crt0.s).
| Responsibilities:
|   * init MD VDP (planes blank, display on) and the controller ports
|   * every VBlank: read both pads (3/6-button) into COMM8 / COMM10
|   * main loop runs from work RAM (keeps the cartridge bus free for the
|     SH-2s) and services simple requests from the master SH-2 in COMM0.
| ---------------------------------------------------------------------------

        .text

| 0x880800 - entry point for reset/cold-start
        .global _start
_start:
| Clear Work RAM
        moveq   #0,d0
        move.w  #0x3FFF,d1
        suba.l  a1,a1
1:
        move.l  d0,-(a1)
        dbra    d1,1b

| Copy initialized variables (and the RAM-resident main loop) to Work RAM
        lea     __text_end,a0
        move.w  #__data_size,d0
        lsr.w   #1,d0
        subq.w  #1,d0
        bmi.b   3f
2:
        move.w  (a0)+,(a1)+
        dbra    d0,2b
3:
        lea     __stack,sp              /* stack at top of Work RAM */
        bsr     init_hardware
        jmp     do_main

        .align  64

| 0x880840 - 68000 general exception handler
        move.l  d0,-(sp)
        move.l  (sp)+,d0
        addq.l  #4,sp                   /* pop jump table return address */
        rte

        .align  64

| 0x880880 - 68000 Level 4 interrupt handler - HBlank IRQ
        rte

        .align  64

| 0x8808C0 - 68000 Level 6 interrupt handler - VBlank IRQ
        movem.l d0-d2/a0,-(sp)
        move.w  0xC00004,d0             /* ack VDP */
        lea     0xA10003,a0
        bsr     get_pad
        move.w  d2,0xA15128             /* COMM8  = pad 1 */
        lea     0xA10005,a0
        bsr     get_pad
        move.w  d2,0xA1512A             /* COMM10 = pad 2 */
        addq.w  #1,vbl_count
        move.w  0xA1512C,0xFFF000       /* telemetry: COMM12 -> work RAM */
        move.w  0xA1512E,0xFFF002       /* telemetry: COMM14 -> work RAM */
        move.w  vbl_count,0xFFF004
        movem.l (sp)+,d0-d2/a0
        rte

| ---------------------------------------------------------------------------
init_hardware:
        lea     0xC00004,a0
        move.w  #0x8104,(a0)            /* display off, vblank disabled */
        move.w  (a0),d0

| init joyports (TH output)
        move.b  #0x40,0xA10009
        move.b  #0x40,0xA1000B
        move.b  #0x40,0xA10003
        move.b  #0x40,0xA10005

| init MD VDP
        move.w  #0x8004,(a0) /* reg. 0 - Disable HBL INT */
        move.w  #0x8174,(a0) /* reg. 1 - Enable display, VBL INT, DMA + 28 VCell size */
        move.w  #0x8230,(a0) /* reg. 2 - Plane A =$30*$400=$C000 */
        move.w  #0x832C,(a0) /* reg. 3 - Window  =$2C*$400=$B000 */
        move.w  #0x8407,(a0) /* reg. 4 - Plane B =$7*$2000=$E000 */
        move.w  #0x855E,(a0) /* reg. 5 - sprite table begins at $BC00=$5E*$200 */
        move.w  #0x8600,(a0) /* reg. 6 - not used */
        move.w  #0x8700,(a0) /* reg. 7 - Background Color number */
        move.w  #0x8800,(a0) /* reg. 8 - not used */
        move.w  #0x8900,(a0) /* reg. 9 - not used */
        move.w  #0x8A01,(a0) /* reg 10 - HInterrupt timing */
        move.w  #0x8B00,(a0) /* reg 11 - $0000abcd a=extr.int b=vscr cd=hscr */
        move.w  #0x8C81,(a0) /* reg 12 - 40 cell, no shadow, no interlace */
        move.w  #0x8D2E,(a0) /* reg 13 - HScroll Table = $B800 */
        move.w  #0x8E00,(a0) /* reg 14 - not used */
        move.w  #0x8F02,(a0) /* reg 15 - auto increment data */
        move.w  #0x9011,(a0) /* reg 16 - scrl screen v&h size (64x64) */
        move.w  #0x9100,(a0) /* reg 17 - window hpos */
        move.w  #0x92FF,(a0) /* reg 18 - window vpos */

| clear VRAM (blank planes, sprite table) so nothing covers the 32X layer
        move.l  #0x40000000,(a0)        /* VRAM write, address 0 */
        lea     0xC00000,a1
        move.w  #0x7FFF,d1
        moveq   #0,d0
4:
        move.w  d0,(a1)
        dbra    d1,4b
| clear CRAM (backdrop = colour 0 = black)
        move.l  #0xC0000000,(a0)
        move.w  #63,d1
5:
        move.w  d0,(a1)
        dbra    d1,5b
| clear VSRAM
        move.l  #0x40000010,(a0)
        move.w  #39,d1
6:
        move.w  d0,(a1)
        dbra    d1,6b

        move.w  #0xF000,0xA15128        /* no pad yet */
        move.w  #0xF000,0xA1512A

| allow the 68k to access the FM chip (Z80 held on the bus request)
        move.w  #0x0100,0xA11100        /* Z80 assert bus request */
        move.w  #0x0100,0xA11200        /* Z80 deassert reset */

| silence the PSG
        lea     0xC00011,a1
        move.b  #0x9F,(a1)
        move.b  #0xBF,(a1)
        move.b  #0xDF,(a1)
        move.b  #0xFF,(a1)

| wait on Mars side
        bclr    #0,0xA15107             /* clear RV - allow SH2 to access ROM */
7:
        cmp.l   #0x4D5F4F4B,0xA15120    /* M_OK */
        bne.b   7b
8:
        cmp.l   #0x535F4F4B,0xA15124    /* S_OK */
        bne.b   8b
        rts

| ---------------------------------------------------------------------------
| get current pad value
| entry: a0 = pad control port
| exit:  d2 = pad value (0 0 0 1 M X Y Z S A C B R L D U) or
|             (0 0 0 0 0 0 0 0 S A C B R L D U) or 0xF000 (none)
get_pad:
        bsr.b   get_input       /* - 0 s a 0 0 d u - 1 c b r l d u */
        move.w  d0,d1
        andi.w  #0x0C00,d0
        bne.b   no_pad
        bsr.b   get_input       /* - 0 s a 0 0 d u - 1 c b r l d u */
        bsr.b   get_input       /* - 0 s a 0 0 0 0 - 1 c b m x y z */
        move.w  d0,d2
        bsr.b   get_input       /* - 0 s a 1 1 1 1 - 1 c b r l d u */
        andi.w  #0x0F00,d0
        cmpi.w  #0x0F00,d0
        beq.b   common          /* six button pad */
        move.w  #0x010F,d2      /* three button pad */
common:
        lsl.b   #4,d2
        lsl.w   #4,d2
        andi.w  #0x303F,d1
        move.b  d1,d2
        lsr.w   #6,d1
        or.w    d1,d2
        eori.w  #0x1FFF,d2
        rts
no_pad:
        move.w  #0xF000,d2
        rts

get_input:
        move.b  #0x00,(a0)
        nop
        nop
        move.b  (a0),d0
        move.b  #0x40,(a0)
        lsl.w   #8,d0
        move.b  (a0),d0
        rts

| ---------------------------------------------------------------------------
        .data
| Main loop lives in Work RAM to lower cartridge bus contention.

        .global do_main
do_main:
        move.w  0xA15100,d0
        or.w    #0x8000,d0
        move.w  d0,0xA15100             /* set FM - allow SH2 access to MARS hw */
        move.l  #0,0xA15120             /* let Master SH2 run */
        move.w  #0x2000,sr              /* enable interrupts */
main_loop:
        move.w  0xA15120,d0             /* COMM0 = request from master SH-2 */
        beq.b   main_loop
        cmpi.w  #0x0100,d0              /* 0x01xx: FM register write (part = xx&1) */
        bne.b   9f
        move.w  0xA15122,d1             /* COMM2 = reg<<8 | value */
        move.w  d1,d2
        lsr.w   #8,d2
        move.b  d2,0xA04000
        nop
        nop
        move.b  d1,0xA04001
9:
        move.w  #0,0xA15120             /* done */
        bra.b   main_loop

        .global vbl_count
vbl_count:
        .word   0
        .word   0
        .long   0x44554B45              /* keep >= 16 bytes of initialised data */
        .long   0x33443332
        .long   0
