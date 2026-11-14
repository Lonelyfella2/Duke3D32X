/*
 * hud_text.c - in-game text in the 8x8 bevelled font from assets/font.png
 * (converted by tools/mkfont.py into gen/font8x8.h).
 *
 * Text is drawn at the 32X's native 320x224 resolution.  The 3D view is
 * rendered at 160x96 and the line table shows every row twice, so the
 * platform layer hands out private, un-doubled copies of the view lines a
 * string touches (plat_text_line) - the text stays crisp over the view.
 *
 * Each glyph pixel is a shadow, body or highlight pixel; a style maps those
 * three roles to palette indices so the bevel survives any recolouring
 * (e.g. yellow text = white highlight, yellow body, orange shadow).
 */
#include "game.h"
#include "font8x8.h"
#include "dukefont.h"

/* target colours (r,g,b) per style: shadow, body, highlight */
static const uint8_t style_rgb[TS_COUNT][3][3] = {
    [TS_YELLOW] = { { 176,  72,   0 }, { 252, 212,  32 }, { 255, 255, 255 } },
    [TS_BLUE]   = { {  32,  32,  66 }, {  66,  82, 138 }, {  95, 205, 228 } },
    [TS_RED]    = { {  96,   0,   0 }, { 216,  32,  16 }, { 255, 200,  96 } },
    [TS_GRAY]   = { {  56,  56,  56 }, { 168, 168, 168 }, { 255, 255, 255 } },
    [TS_GREEN]  = { {   0,  64,   0 }, {  40, 180,  40 }, { 200, 255, 160 } },
};
static uint8_t style_idx[TS_COUNT][4];      /* [role] -> palette index */
static const uint16_t *style_cram;          /* palette style_idx was matched to */
static const uint16_t *text_cram;           /* palette on screen (0 = Duke's)  */

static int nearest_colour_in(const uint16_t *cram, int r, int g, int b)
{
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 255; i++) {          /* 255 is the transparent index */
        uint16_t v = cram[i];
        int cr = (v & 31) << 3, cg = ((v >> 5) & 31) << 3, cb = ((v >> 10) & 31) << 3;
        int d = (cr - r) * (cr - r) * 3 + (cg - g) * (cg - g) * 4 + (cb - b) * (cb - b) * 2;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* match the styles to the palette on screen (Duke's, or a full-screen
 * picture's own palette, which convert.py extends with these colours) */
static void styles_init(const uint16_t *cram)
{
    for (int s = 0; s < TS_COUNT; s++) {
        style_idx[s][0] = 0;
        for (int k = 0; k < 3; k++)
            style_idx[s][k + 1] = (uint8_t)nearest_colour_in(cram, style_rgb[s][k][0], style_rgb[s][k][1], style_rgb[s][k][2]);
    }
    style_cram = cram;
}

void txt_palette(const uint16_t *cram) { text_cram = cram; }

static int glyph_of(int ch)
{
    ch &= 0xff;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) return '?' - FONT_FIRST;
    return ch - FONT_FIRST;
}

int txt_width(const char *s)
{
    int w = 0;
    for (; *s; s++) w += font_adv[glyph_of((uint8_t)*s)];
    return w > 0 ? w - 1 : 0;                /* no gap after the last glyph */
}

/* x, y: native screen pixels (320x224); draws rows that fall on screen */
void txt_draw(int x, int y, const char *s, int style)
{
    const uint16_t *cram = text_cram ? text_cram : dk_cram;
    if (cram != style_cram) styles_init(cram);
    const uint8_t *col = style_idx[(unsigned)style < TS_COUNT ? style : 0];
    for (int r = 0; r < FONT_H; r++) {
        uint8_t *line = plat_text_line(y + r);
        if (!line) continue;
        int cx = x;
        for (const char *p = s; *p; p++) {
            int g = glyph_of((uint8_t)*p);
            uint32_t bits = font_rows[g][r];
            for (int c = 0; bits; c++, bits >>= 2) {
                int k = (int)(bits & 3);
                if (k && (unsigned)(cx + c) < 320u) line[cx + c] = col[k];
            }
            cx += font_adv[g];
            if (cx >= 320) break;
        }
    }
}

void txt_center(int y, const char *s, int style)
{
    txt_draw((320 - txt_width(s)) >> 1, y, s, style);
}

/* Centre a long line, wrapping once at a space if it is wider than maxw.
 * Returns the number of text rows used (1 or 2). */
int txt_center_wrap(int y, const char *s, int style, int maxw)
{
    if (txt_width(s) <= maxw) { txt_center(y, s, style); return 1; }
    int n = (int)strlen(s), best = -1;
    char a[64], b[64];
    for (int i = 1; i < n && i < 63; i++) {   /* the last space that still fits */
        if (s[i] != ' ') continue;
        memcpy(a, s, (size_t)i); a[i] = 0;
        if (txt_width(a) <= maxw) best = i;
    }
    if (best < 0) { txt_center(y, s, style); return 1; }
    memcpy(a, s, (size_t)best); a[best] = 0;
    int m = n - best - 1; if (m > 63) m = 63;
    memcpy(b, s + best + 1, (size_t)m); b[m] = 0;
    txt_center(y, a, style);
    txt_center(y + FONT_H + 2, b, style);
    return 2;
}

/* native-resolution fill (HUD / screen lines) */
void txt_fill(int x, int y, int w, int h, int colour)
{
    for (int r = 0; r < h; r++) {
        uint8_t *line = plat_text_line(y + r);
        if (!line) continue;
        for (int c = 0; c < w; c++)
            if ((unsigned)(x + c) < 320u) line[x + c] = (uint8_t)colour;
    }
}

/* ---------------------------------------------------------------------
 * Big bitmap fonts converted by tools/mkbigfont.py: Duke's big red menu font
 * (assets/dukefont.png: title, pause screen) and the Tormentstein 3D font
 * (assets/tormentfont.png: top messages, ENTERING / LEVEL COMPLETED, the
 * skill and level menu).  Glyph pixels keep the sheet's own colours, scaled
 * by a tint, and are matched to whichever palette is on screen (a picture's
 * palette carries the exact colours, see convert.py). */
#include "tormentfont.h"

typedef struct {
    int h, ncol;
    const uint8_t (*rgb)[3];
    const bigfont_glyph_t *glyph;
    const uint8_t *px;
} bigfont_t;

static const bigfont_t font_duke = { DFONT_H, DFONT_NCOL, dfont_rgb, dfont_glyph, dfont_px };
static const bigfont_t font_torment = { TFONT_H, TFONT_NCOL, tfont_rgb, tfont_glyph, tfont_px };

/* TF_* tints (keep in sync with TFONT_TINTS in tools/convert.py) */
static const uint8_t tint_rgb[TF_COUNT][3] = {
    [TF_WHITE]  = { 255, 255, 255 },
    [TF_YELLOW] = { 255, 214, 64 },
    [TF_RED]    = { 255, 96, 72 },
    [TF_BLUE]   = { 150, 180, 255 },
};

typedef struct { const bigfont_t *f; const uint16_t *cram; int tint; uint8_t map[16]; } fontmap_t;
static fontmap_t fmaps[6];
static int fmap_next;

static const uint8_t *font_map(const bigfont_t *f, const uint16_t *cram, int tint)
{
    for (int i = 0; i < 6; i++)
        if (fmaps[i].f == f && fmaps[i].cram == cram && fmaps[i].tint == tint) return fmaps[i].map;
    fontmap_t *m = &fmaps[fmap_next];
    fmap_next = (fmap_next + 1) % 6;
    m->f = f; m->cram = cram; m->tint = tint; m->map[0] = 0;
    const uint8_t *t = tint_rgb[(unsigned)tint < TF_COUNT ? tint : 0];
    for (int k = 0; k < f->ncol; k++)
        m->map[k + 1] = (uint8_t)nearest_colour_in(cram, f->rgb[k][0] * t[0] / 255,
                                                   f->rgb[k][1] * t[1] / 255, f->rgb[k][2] * t[2] / 255);
    return m->map;
}

static int bigfont_index(int ch)
{
    ch &= 0xff;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    return (ch >= 32 && ch < 96) ? ch - 32 : -1;
}

static int bigfont_width(const bigfont_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int g = bigfont_index((uint8_t)*s);
        if (g >= 0) w += f->glyph[g].adv;
    }
    return w;
}

static void bigfont_draw(const bigfont_t *f, int x, int y, const char *s, const uint16_t *cram, int tint)
{
    const uint8_t *map = font_map(f, cram, tint);
    int x0 = 320, x1 = 0, cx = x;
    for (const char *p = s; *p; p++) {
        int g = bigfont_index((uint8_t)*p);
        if (g < 0) continue;
        int gw = f->glyph[g].w;
        if (gw > 0) {
            if (cx < x0) x0 = cx;
            if (cx + gw > x1) x1 = cx + gw;
        }
        cx += f->glyph[g].adv;
    }
    if (x0 < 0) x0 = 0;
    if (x1 > 320) x1 = 320;
    if (x0 >= x1) return;
    int r0 = y < 0 ? -y : 0;
    int r1 = (y + f->h > 224) ? (224 - y) : f->h;
    if (r0 >= r1) return;
    int w0 = x0 >> 2, w1 = (x1 + 3) >> 2, nw = w1 - w0, xbase = w0 << 2;
    int nr = r1 - r0, rowbytes = nw << 2;
    uint32_t buf[15 * 80];
    uint8_t *rows[15];
    if (nr > 15) nr = 15;
    if (y + r0 + nr <= 192) {
        uint8_t *base = plat_text_span(y + r0, y + r0 + nr);
        for (int r = 0; r < nr; r++) rows[r] = base + r * 320;
    } else {
        for (int r = 0; r < nr; r++) rows[r] = plat_text_line(y + r0 + r);
    }
    uint8_t *rb = (uint8_t *)buf;
    for (int r = 0; r < nr; r++) {
        uint8_t *line = rows[r];
        if (!line) continue;
        volatile uint32_t *l32 = (volatile uint32_t *)line + w0;
        uint32_t *dst32 = buf + r * nw;
        for (int k = 0; k < nw; k++) dst32[k] = l32[k];
    }
    cx = x;
    for (const char *p = s; *p; p++) {
        int g = bigfont_index((uint8_t)*p);
        if (g < 0) continue;
        int gw = f->glyph[g].w, adv = f->glyph[g].adv;
        if (gw > 0 && cx + gw > x0 && cx < x1) {
            int bw = (gw + 1) >> 1;
            const uint8_t *src = f->px + f->glyph[g].off + r0 * bw;
            int c0 = cx < 0 ? -cx : 0;
            int c1 = cx + gw > 320 ? 320 - cx : gw;
            uint8_t *dr = rb + (cx - xbase);
            for (int r = 0; r < nr; r++, src += bw, dr += rowbytes) {
                if (!rows[r]) continue;
                for (int c = c0; c < c1; ) {
                    if ((c & 1) == 0 && c + 1 < c1) {
                        uint8_t b = src[c >> 1];
                        if (b) {
                            uint8_t lo = b & 15, hi = b >> 4;
                            if (lo) dr[c] = map[lo];
                            if (hi) dr[c + 1] = map[hi];
                        }
                        c += 2;
                    } else {
                        int v = (c & 1) ? (src[c >> 1] >> 4) : (src[c >> 1] & 15);
                        if (v) dr[c] = map[v];
                        c++;
                    }
                }
            }
        }
        cx += adv;
    }
    for (int r = 0; r < nr; r++) {
        uint8_t *line = rows[r];
        if (!line) continue;
        volatile uint32_t *l32 = (volatile uint32_t *)line + w0;
        const uint32_t *src32 = buf + r * nw;
        for (int k = 0; k < nw; k++) l32[k] = src32[k];
    }
}

/* Duke's big red font: title and pause screens */
int dfont_width(const char *s) { return bigfont_width(&font_duke, s); }
void dfont_draw(int x, int y, const char *s, const uint16_t *cram) { bigfont_draw(&font_duke, x, y, s, cram, TF_WHITE); }
void dfont_center(int y, const char *s, const uint16_t *cram)
{
    dfont_draw((320 - dfont_width(s)) >> 1, y, s, cram);
}

/* Tormentstein 3D font, on the palette set by txt_palette() */
int tfont_width(const char *s) { return bigfont_width(&font_torment, s); }
void tfont_draw(int x, int y, const char *s, int tint)
{
    bigfont_draw(&font_torment, x, y, s, text_cram ? text_cram : dk_cram, tint);
}
void tfont_center(int y, const char *s, int tint) { tfont_draw((320 - tfont_width(s)) >> 1, y, s, tint); }

/* centred, wrapped once at a space when wider than maxw; returns the rows */
int tfont_center_wrap(int y, const char *s, int tint, int maxw)
{
    static const char *cw_s;
    static int cw_maxw;
    static char cw_a[64], cw_b[64];
    static int16_t cw_x1, cw_x2, cw_n;
    if (cw_s != s || cw_maxw != maxw) {
        cw_s = s; cw_maxw = maxw;
        int w = tfont_width(s);
        if (w <= maxw) {
            cw_n = 1;
            cw_x1 = (int16_t)((320 - w) >> 1);
        } else {
            int n = (int)strlen(s), best = -1;
            for (int i = 1; i < n && i < 63; i++) {
                if (s[i] != ' ') continue;
                memcpy(cw_a, s, (size_t)i); cw_a[i] = 0;
                if (tfont_width(cw_a) <= maxw) best = i;
            }
            if (best < 0) {
                cw_n = 1;
                cw_x1 = (int16_t)((320 - w) >> 1);
            } else {
                memcpy(cw_a, s, (size_t)best); cw_a[best] = 0;
                int m = n - best - 1; if (m > 63) m = 63;
                memcpy(cw_b, s + best + 1, (size_t)m); cw_b[m] = 0;
                cw_n = 2;
                cw_x1 = (int16_t)((320 - tfont_width(cw_a)) >> 1);
                cw_x2 = (int16_t)((320 - tfont_width(cw_b)) >> 1);
            }
        }
    }
    if (cw_n == 1) { tfont_draw(cw_x1, y, s, tint); return 1; }
    tfont_draw(cw_x1, y, cw_a, tint);
    tfont_draw(cw_x2, y + TFONT_LINE, cw_b, tint);
    return 2;
}

/* a 2D tile at native resolution (top-left at x,y), e.g. Duke's spinning
 * nuke menu cursor; colours are Duke palette indices */
void txt_pic(int x, int y, int pic)
{
    if (!tile_present(pic)) return;
    const dk_tile_t *t = &tiledir[pic];
    const uint8_t *px = (const uint8_t *)DKPTR(t->off);
    int sc = t->shift ? 2 : 1;
    for (int r = 0; r < t->h * sc; r++) {
        uint8_t *line = plat_text_line(y + r);
        if (!line) continue;
        for (int c = 0; c < t->w * sc; c++) {
            uint8_t v = px[(c / sc) * t->h + r / sc];
            if (v != 255 && (unsigned)(x + c) < 320u) line[x + c] = v;
        }
    }
}
