/*
 * main32x.c - Duke Nukem 3D 32X: SH-2 entry points.
 *
 * Master SH-2: game logic + left band of the 3D view + HUD + flips.
 * Slave  SH-2: right band of the 3D view + PWM mixer (DMA IRQ driven).
 */
#include <stdint.h>
#include "hw32x.h"
#include "render.h"
#include "game.h"
#include "sound32x.h"

extern const uint8_t dk_gamedata[];


static inline void wdt_start7(void)
{
    *(volatile uint16_t *)0xFFFFFE80 = 0xA527; /* IT mode, TME=1, CKS=7 (Fs/8192 ~ 356us/tick) */
    *(volatile uint16_t *)0xFFFFFE80 = 0x5A00;
}
static inline uint8_t wdt_read(void) { return *(volatile uint8_t *)0xFFFFFE81; }

/* master -> slave job, always accessed through the cache-through mirror */
typedef struct {
    int32_t x0, x1;
    int32_t frame;
    int32_t ticks;          /* slave render time in WDT ticks */
} slavejob_t;
static slavejob_t slavejob;
#define SJ ((volatile slavejob_t *)((uintptr_t)&slavejob | 0x20000000))

#define SCMD_RENDER   1
#define SCMD_SNDINIT  2

void secondary(void)
{
    ClearCache();
    for (;;) {
        int cmd;
        while ((cmd = MARS_SYS_COMM4) == 0) {}
        switch (cmd) {
        case SCMD_RENDER:
            CacheControl(SH2_CCTL_CP | SH2_CCTL_CE);    /* see master's world updates */
            wdt_start7();
            render_frame_s(SJ->x0, SJ->x1);
            SJ->ticks = (int32_t)wdt_read();
            break;
        case SCMD_SNDINIT:
            snd_slave_init();
            break;
        default:
            break;
        }
        MARS_SYS_COMM4 = 0;
    }
}

/* ---- called by the game ---- */
static int32_t split_x = VIEW_W / 2;
static rstats_t mirror_seen = { .mirror_wall = -1 };
static int32_t render_tm;

void plat_render_begin(void)
{
    rview.frame = (pixel_t *)(0x24000000 + FB_VIEW_OFS * 2);
    rview.pitch = FB_PITCH16;
    rview.xdim = VIEW_W;
    rview.ydim = VIEW_H;
    if (mirror_seen.mirror_wall >= 0) render_mirror(&mirror_seen);
#ifdef MASTER_ONLY
    render_frame_m(0, VIEW_W - 1);
    return;
#endif
    SJ->x0 = split_x;
    SJ->x1 = VIEW_W - 1;
    MARS_SYS_COMM4 = SCMD_RENDER;
    wdt_start7();
    render_frame_m(0, split_x - 1);
    render_tm = (int32_t)wdt_read();
}

void plat_render_end(void)
{
#ifdef MASTER_ONLY
    return;
#endif
    int32_t tm = render_tm;
    while (MARS_SYS_COMM4 != 0) {}
    int32_t ts = SJ->ticks;
    {   /* slave stats via the cache-through mirror */
        rstats_t sl = *(volatile rstats_t *)((uintptr_t)&rstats_s | 0x20000000);
        render_merge_mirror(&mirror_seen, &rstats_m, &sl);
    }
    int32_t diff = ts - tm;
    if (diff > 2 || diff < -2) {
        int32_t step = div32(diff * 48, tm + ts + 12);
        if (step == 0) step = (diff > 0) ? 1 : -1;
        if (step > 28) step = 28;
        else if (step < -28) step = -28;
        split_x += step;
        if (split_x < 12) split_x = 12;
        if (split_x > VIEW_W - 12) split_x = VIEW_W - 12;
    }
}

void plat_render_view(void)
{
    plat_render_begin();
    plat_render_end();
}

pixel_t *plat_hud_base(void) { return (pixel_t *)(0x24000000 + FB_HUD_OFS * 2); }
pixel_t *plat_view_base(void) { return (pixel_t *)(0x24000000 + FB_VIEW_OFS * 2); }

/* ---- native-resolution text over the line-doubled view ----
 * Each framebuffer has 192 spare lines after the HUD (FB_TXT_OFS).  When a
 * text row lands on screen line sy of the view, the (doubled) view row is
 * copied there and the line table entry for sy is pointed at the copy; the
 * text is then drawn at full 320x224 resolution.  plat_frame_begin() (called
 * before drawing each new frame) resets the back buffer's entries to the
 * doubled view rows; static screens that are only re-flipped keep theirs. */
#define FB_TXT_OFS (FB_HUD_OFS + HUD_LINES * FB_PITCH16)
static uint8_t txt_line_on[VIEW_H * 2];
static int txt_any_used = 2;

uint8_t *plat_text_span(int y0, int y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > VIEW_H * 2) y1 = VIEW_H * 2;
    if (y0 >= y1) return (uint8_t *)(0x24000000 + (FB_TXT_OFS + (uint32_t)y0 * FB_PITCH16) * 2);
    int y = y0;
    while (y < y1) {
        if ((y & 1) == 0 && (y + 1) < y1 && !txt_line_on[y] && !txt_line_on[y + 1]) {
            uint32_t ofs0 = FB_TXT_OFS + (uint32_t)y * FB_PITCH16;
            uint32_t ofs1 = ofs0 + FB_PITCH16;
            const volatile uint32_t *src = (const volatile uint32_t *)(0x24000000 + (FB_VIEW_OFS + (y >> 1) * FB_PITCH16) * 2);
            volatile uint32_t *d0 = (volatile uint32_t *)(0x24000000 + ofs0 * 2);
            volatile uint32_t *d1 = (volatile uint32_t *)(0x24000000 + ofs1 * 2);
            for (int i = 0; i < 80; i += 4) {
                uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
                d0[i] = a; d0[i + 1] = b; d0[i + 2] = c; d0[i + 3] = d;
                d1[i] = a; d1[i + 1] = b; d1[i + 2] = c; d1[i + 3] = d;
            }
            FB16[y] = (uint16_t)ofs0;
            FB16[y + 1] = (uint16_t)ofs1;
            txt_line_on[y] = 1;
            txt_line_on[y + 1] = 1;
            txt_any_used = 2;
            y += 2;
        } else {
            if (!txt_line_on[y]) {
                uint32_t ofs = FB_TXT_OFS + (uint32_t)y * FB_PITCH16;
                const volatile uint32_t *src = (const volatile uint32_t *)(0x24000000 + (FB_VIEW_OFS + (y >> 1) * FB_PITCH16) * 2);
                volatile uint32_t *dst = (volatile uint32_t *)(0x24000000 + ofs * 2);
                for (int i = 0; i < 80; i += 4) {
                    uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
                    dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d;
                }
                FB16[y] = (uint16_t)ofs;
                txt_line_on[y] = 1;
                txt_any_used = 2;
            }
            y++;
        }
    }
    return (uint8_t *)(0x24000000 + (FB_TXT_OFS + (uint32_t)y0 * FB_PITCH16) * 2);
}

uint8_t *plat_text_line(int sy)
{
    if ((unsigned)sy >= 224u) return 0;
    if (sy >= VIEW_H * 2)
        return (uint8_t *)(0x24000000 + (FB_HUD_OFS + (sy - VIEW_H * 2) * FB_PITCH16) * 2);
    uint32_t ofs = FB_TXT_OFS + (uint32_t)sy * FB_PITCH16;
    if (!txt_line_on[sy]) {
        const volatile uint32_t *src = (const volatile uint32_t *)(0x24000000 + (FB_VIEW_OFS + (sy >> 1) * FB_PITCH16) * 2);
        volatile uint32_t *dst = (volatile uint32_t *)(0x24000000 + ofs * 2);
        for (int i = 0; i < 80; i += 4) {
            uint32_t a = src[i], b = src[i + 1], c = src[i + 2], d = src[i + 3];
            dst[i] = a; dst[i + 1] = b; dst[i + 2] = c; dst[i + 3] = d;
        }
        FB16[sy] = (uint16_t)ofs;
        txt_line_on[sy] = 1;
        txt_any_used = 2;
    }
    return (uint8_t *)(0x24000000 + ofs * 2);
}

uint8_t *plat_screen_line(int sy)
{
    if ((unsigned)sy >= 224u) return 0;
    if (sy >= VIEW_H * 2)
        return (uint8_t *)(0x24000000 + (FB_HUD_OFS + (sy - VIEW_H * 2) * FB_PITCH16) * 2);
    uint32_t ofs = FB_TXT_OFS + (uint32_t)sy * FB_PITCH16;
    if (!txt_line_on[sy]) {
        FB16[sy] = (uint16_t)ofs;
        txt_line_on[sy] = 1;
        txt_any_used = 2;
    }
    return (uint8_t *)(0x24000000 + ofs * 2);
}

static void txt_lines_reset(void)
{
    if (!txt_any_used) return;
    txt_any_used--;
    volatile uint16_t *lt = FB16;
    for (int i = 0; i < VIEW_H * 2; i++) {
        lt[i] = (uint16_t)(FB_VIEW_OFS + (i >> 1) * FB_PITCH16);
        txt_line_on[i] = 0;
    }
}

uint32_t plat_ticks(void) { return mars_vblank_count; }
int plat_pal(void) { return hw_is_pal(); }

uint32_t plat_read_input(void)
{
    return hw_read_pad(0);
}

void plat_set_palette(const uint16_t *cram) { hw_set_palette(cram, 0, 256); }
void plat_set_palette_async(const uint16_t *cram) { hw_set_palette_async(cram, 0, 256); }

/* sound: forwarded to the slave SH-2 mixer */
void plat_sound_play(int id, int vol, int pan, int32_t pitch, int loop, int tag) { snd_play(id, vol, pan, pitch, loop, tag); }
void plat_sound_stop_tag(int tag) { snd_stop_tag(tag); }
void plat_sound_stop_all(void) { snd_stop_all(); }
int plat_sound_active(int id) { return snd_active(id); }
void plat_flip(void) { hw_flip(); }
void plat_flip_request(void) { hw_flip_request(); }
void plat_flip_wait(void) { hw_flip_wait(); }
void plat_frame_begin(void) { txt_lines_reset(); }
void plat_telemetry(uint16_t a, uint16_t b) { MARS_SYS_COMM12 = a; MARS_SYS_COMM14 = b; }

int main(void)
{
    hw_init();
    MARS_SYS_COMM12 = 0x0001;            /* telemetry: booted */
    if (engine_init(dk_gamedata) != 0) {
        MARS_SYS_COMM12 = 0xDEAD;
        for (;;) {}
    }
    MARS_SYS_COMM12 = 0x0002;
    render_init();
    MARS_SYS_COMM4 = SCMD_SNDINIT;
    while (MARS_SYS_COMM4 != 0) {}
    MARS_SYS_COMM12 = 0x0003;
    game_main();                          /* never returns */
    return 0;
}
