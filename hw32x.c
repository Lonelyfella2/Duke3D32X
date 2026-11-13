/*
 * hw32x.c - 32X video/system helpers (after Chilly Willy's hw_32x.c).
 */
#include "hw32x.h"

volatile uint32_t mars_vblank_count = 0;
volatile unsigned mars_pwdt_ovf_count = 0;
volatile unsigned mars_swdt_ovf_count = 0;
static volatile uint16_t current_fb = 0;
static uint16_t pending_cram[256] __attribute__((aligned(4)));
static volatile int cram_pending = 0;

void pri_vbi_handler(void);
void pri_cmd_handler(void);
void sec_cmd_handler(void);

void pri_vbi_handler(void)
{
    mars_vblank_count++;
    if (cram_pending && (MARS_VDP_FBCTL & MARS_VDP_VBLK)) {
        volatile uint16_t *cram = &MARS_CRAM;
        const uint16_t *src = pending_cram;
        for (int i = 0; i < 256; i += 4) {
            cram[i]     = src[i];
            cram[i + 1] = src[i + 1];
            cram[i + 2] = src[i + 2];
            cram[i + 3] = src[i + 3];
        }
        cram_pending = 0;
    }
}

void pri_cmd_handler(void)
{
    MARS_SYS_COMM0 = 0xA55A;
    while (MARS_SYS_COMM0 == 0xA55A);
}

void sec_cmd_handler(void)
{
    MARS_SYS_COMM4 = 0xA55A;
    while (MARS_SYS_COMM4 == 0xA55A);
}

uint32_t hw_ticks(void) { return mars_vblank_count; }

int hw_is_pal(void) { return (MARS_VDP_DISPMODE & MARS_NTSC_FORMAT) == 0; }

static void setup_linetable(void)
{
    volatile uint16_t *fb = FB16;
    for (int i = 0; i < 224; i++) {
        if (i < VIEW_H * 2)
            fb[i] = (uint16_t)(FB_VIEW_OFS + (i >> 1) * FB_PITCH16);
        else
            fb[i] = (uint16_t)(FB_HUD_OFS + (i - VIEW_H * 2) * FB_PITCH16);
    }
    for (int i = 224; i < 256; i++) fb[i] = (uint16_t)FB_HUD_OFS;
    /* clear the pixel area with 32-bit stores (overwrite image drops 0s) */
    volatile uint32_t *p = (volatile uint32_t *)(0x24000000 + 0x200);
    for (int i = 0; i < ((VIEW_H + HUD_LINES) * 320) / 4; i++) p[i] = 0;
}

static volatile int flip_pending = 0;

void hw_flip_request(void)
{
    if (flip_pending) hw_flip_wait();
    MARS_VDP_FBCTL = current_fb ^ 1;
    flip_pending = 1;
}

void hw_flip_wait(void)
{
    if (!flip_pending) return;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == current_fb);
    current_fb ^= 1;
    flip_pending = 0;
}

static void flip_now(void)
{
    hw_flip_request();
    hw_flip_wait();
}

void hw_init(void)
{
    while ((MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) == 0);
    MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
    current_fb = MARS_VDP_FBCTL & MARS_VDP_FS;
    /* both buffers: line table + clear */
    setup_linetable();
    flip_now();
    setup_linetable();
    flip_now();
    /* seed a visible palette before the first real one arrives */
    volatile uint16_t *cram = &MARS_CRAM;
    for (int i = 0; i < 256; i++) {
        uint16_t v = 0x8000 | (uint16_t)((i >> 3) * 0x421);
        cram[i] = v;
        pending_cram[i] = v;
    }
}

void hw_set_palette(const uint16_t *cram, int first, int count)
{
    cram_pending = 0;
    /* CRAM is accessible during VBlank: wait for it and copy directly */
    while (!(MARS_VDP_FBCTL & MARS_VDP_VBLK)) {}
    volatile uint16_t *c = &MARS_CRAM;
    for (int i = 0; i < count; i++) c[first + i] = cram[i];
}

void hw_set_palette_async(const uint16_t *cram, int first, int count)
{
    cram_pending = 0;
    for (int i = 0; i < count; i++) pending_cram[first + i] = cram[i];
    cram_pending = 1;
}
void hw_wait_vblank(void)
{
    uint32_t t = mars_vblank_count;
    while (mars_vblank_count == t);
}

void hw_flip(void)
{
    flip_now();
}

uint16_t hw_read_pad(int port)
{
    uint16_t v = port ? MARS_SYS_COMM10 : MARS_SYS_COMM8;
    if ((v & SEGA_CTRL_TYPE) == SEGA_CTRL_NONE) return 0;
    return v;
}
