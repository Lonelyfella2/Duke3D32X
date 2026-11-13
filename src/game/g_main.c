/*
 * g_main.c - game flow (title, skill menu, campaign of kept levels,
 * intermission), the 30 Hz fixed-step loop, HUD and screen effects.
 */
#include "g_local.h"
#include "lz4.h"

int32_t gametic, level_index, game_skill = 2;
int32_t earthquaketime;
int32_t visibility_g = CON_DEFAULTVISIBILITY;
int16_t camsprite = -1;
int16_t hud_dirty = 2;

static uint16_t cur_cram[256];
static int cur_palmode = -1;
/* palette a full-screen picture (or the 2D menus) loaded into CRAM; NULL
 * while the game's own palette (update_palette) is in charge */
static const uint16_t *shown_pal;
static uint32_t pad_now, pad_prev;
static int32_t turnheld;

/* ---------------------------------------------------------- drawing */
/* blit a (half-res stored) tile 1:1 into the doubled 3D view */
static void blit_view(int pic, int x, int y, int shade, int pal, int flip)
{
    if (!tile_present(pic)) return;
    const dk_tile_t *t = &tiledir[pic];
    const uint8_t *px = (const uint8_t *)DKPTR(t->off);
    const uint8_t *pl8 = palookup[pal & 31] + (max(0, min(31, shade)) << 8);
    int step = t->shift ? 1 : 2;     /* full-res tiles: take every 2nd pixel */
    int w = t->w / step, h = t->h / step;
    for (int cx = 0; cx < w; cx++) {
        int dx = x + cx;
        if ((unsigned)dx >= (unsigned)rview.xdim) continue;
        const uint8_t *col = px + (flip ? (w - 1 - cx) : cx) * step * t->h;
        for (int cy = 0; cy < h; cy++) {
            int dy = y + cy;
            if ((unsigned)dy >= (unsigned)rview.ydim) continue;
            uint8_t c = col[cy * step];
            if (c != 255) rview.frame[dy * rview.pitch + dx] = dblpix(pl8[c]);
        }
    }
}

/* Duke's rotatesprite() placement of an unscaled 2D tile on the native
 * 320x32 status bar (y = Duke's screen y - 166, the bar's top row):
 * topleft = flag 16 (no centring), otherwise the tile is centred on (x,y)
 * through its picanm offsets.  shade/pal go through the palookup tables. */
static void hud_pic(uint8_t *hud, int x, int y, int pic, int shade, int pal, int topleft)
{
    if (!tile_present(pic)) return;
    const dk_tile_t *t = &tiledir[pic];
    if (!topleft) { x -= (t->ow >> 1) + t->xoff; y -= (t->oh >> 1) + t->yoff; }
    const uint8_t *px = (const uint8_t *)DKPTR(t->off);
    const uint8_t *lut = palookup[pal & 31] + (max(0, min(31, shade)) << 8);
    int sc = t->shift ? 2 : 1;            /* half-res stored tiles: doubled */
    for (int cx = 0; cx < t->w * sc; cx++) {
        int dx = x + cx;
        if ((unsigned)dx >= 320u) continue;
        const uint8_t *col = px + (cx / sc) * t->h;
        for (int cy = 0; cy < t->h * sc; cy++) {
            int dy = y + cy;
            if ((unsigned)dy >= (unsigned)HUD_LINES_G) continue;
            uint8_t c = col[cy / sc];
            if (c != 255) hud[dy * 320 + dx] = lut[c];
        }
    }
}

#define SBAR_Y(y) ((y) - 166)          /* Duke status-bar coordinates -> HUD row */

/* GAME.C digitalnumber(): red LCD digits, centred on x, top-left at y */
static void hud_digital(uint8_t *hud, int x, int y, int n)
{
    char b[8]; int len = 0, j = 0;
    if (n < 0) n = 0;
    do { b[len++] = (char)(n % 10); n /= 10; } while (n && len < 7);
    for (int k = len - 1; k >= 0; k--) j += TILESIZX(DIGITALNUM + b[k]) + 1;
    int c = x - (j >> 1);
    j = 0;
    for (int k = len - 1; k >= 0; k--) {
        hud_pic(hud, c + j, SBAR_Y(y), DIGITALNUM + b[k], -16, 0, 1);
        j += TILESIZX(DIGITALNUM + b[k]) + 1;
    }
}

/* digits of n (<= 3) drawn with THREEBYFIVE; xs[] are the slots used for
 * 1, 2 or 3 digit numbers (right-aligned layouts of GAME.C) */
static void hud_3x5_number(uint8_t *hud, int n, const int8_t xs[3][3], int x, int y, int shade, int pal)
{
    int d[3], len = 0;
    if (n < 0) n = 0;
    if (n > 999) n = 999;
    if (n > 99) { d[0] = n / 100; d[1] = (n / 10) % 10; d[2] = n % 10; len = 3; }
    else if (n > 9) { d[0] = n / 10; d[1] = n % 10; len = 2; }
    else { d[0] = n; len = 1; }
    for (int k = 0; k < len; k++)
        hud_pic(hud, x + xs[len - 1][k], SBAR_Y(y), THREEBYFIVE + d[k], shade, pal, 0);
}

/* GAME.C weaponnum() / weaponnum999(): "N:ammo/max" in 3x5 digits */
static void hud_weaponnum(uint8_t *hud, int ind, int x, int y, int num1, int num2, int ha, int wide)
{
    static const int8_t n2a[3][3] = { { 4 }, { 0, 4 }, { 0, 4 } };          /* weaponnum (<= 99) */
    static const int8_t n2b[3][3] = { { 13 }, { 13, 17 }, { 13, 17 } };
    static const int8_t w3a[3][3] = { { 8 }, { 4, 8 }, { 0, 4, 8 } };       /* weaponnum999 */
    static const int8_t w3b[3][3] = { { 25 }, { 17, 21 }, { 17, 21, 25 } };
    hud_pic(hud, x - 7, SBAR_Y(y), THREEBYFIVE + ind + 1, ha - 10, 7, 0);
    hud_pic(hud, x - (wide ? 4 : 3), SBAR_Y(y), THREEBYFIVE + 10, ha, 0, 0);    /* ':' */
    hud_pic(hud, x + (wide ? 13 : 9), SBAR_Y(y), THREEBYFIVE + 11, ha, 0, 0);   /* '/' */
    if (!wide) { if (num1 > 99) num1 = 99; if (num2 > 99) num2 = 99; }
    hud_3x5_number(hud, num1, wide ? w3a : n2a, x, y, ha, 0);
    hud_3x5_number(hud, num2, wide ? w3b : n2b, x, y, ha, 0);
}

/* GAME.C minitext(): MINIFONT glyphs, 4 px advance, top-left origin */
static void hud_minitext(uint8_t *hud, int x, int y, const char *t, int pal)
{
    for (; *t; t++) {
        int ch = *t;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch == ' ') { x += 5; continue; }
        hud_pic(hud, x, SBAR_Y(y), MINIFONT + ch - '!', 0, pal, 1);
        x += 4;
    }
}

/* ------------------------------------------------------- palette fx */
static void update_palette(void)
{
    int mode = 0;
    if (sector[pl.cursectnum].lotag == 2) mode = 1;
    if (pl.heat_on) mode = 2;
    int flash = pl.pal_time;
    int want = mode * 64 + (flash ? (32 + (pl.pal_r ? 1 : 0) + (pl.pal_g ? 2 : 0)) : 0) + (flash ? flash : 0);
    if (want == cur_palmode) return;
    cur_palmode = want;
    const uint16_t *src = dk_cram;
    if (mode == 1) src = (const uint16_t *)DKPTR(dk_hdr->off_altpals) + 256 * ALTPAL_WATER;
    if (mode == 2) src = (const uint16_t *)DKPTR(dk_hdr->off_altpals) + 256 * ALTPAL_SLIME;
    for (int c = 0; c < 256; c++) {
        uint16_t v = src[c];
        if (flash) {
            int r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
            int k = flash > 12 ? 12 : flash;      /* up to ~30% tint, fading */
            r += ((pl.pal_r >> 1) - r) * k / 40; g += ((pl.pal_g >> 1) - g) * k / 40; b += ((pl.pal_b >> 1) - b) * k / 40;
            v = (uint16_t)(0x8000 | (b << 10) | (g << 5) | r);
        }
        cur_cram[c] = v | 0x8000;
    }
    plat_set_palette(cur_cram);
    shown_pal = 0;
}

/* ------------------------------------------------------------- HUD */
/* GAME.C coolgaugetext(): Duke's full status bar, same coordinates, so the
 * numbers sit in the middle of the bar's black boxes. */
static const int16_t inv_icons[INV_NUM] = { FIRSTAID_ICON, STEROIDS_ICON, HOLODUKE_ICON, JETPACK_ICON,
                                            HEAT_ICON, AIRTANK_ICON, BOOT_ICON };

static void draw_hud(void)
{
    if (hud_dirty <= 0) return;
    hud_dirty--;
    uint8_t *hud = (uint8_t *)plat_hud_base();
    memset(hud, 0, 320 * HUD_LINES_G);
    hud_pic(hud, 0, 0, BOTTOMSTATUSBAR, 0, 0, 1);        /* bar rows 0..31 (32/33 are blank) */

    /* keys (got_access 4/2/1 = yellow/red/blue cards) */
    if (pl.keys & 4) hud_pic(hud, 275, SBAR_Y(182), ACCESS_ICON, 0, 23, 1);
    if (pl.keys & 2) hud_pic(hud, 288, SBAR_Y(182), ACCESS_ICON, 0, 21, 1);
    if (pl.keys & 1) hud_pic(hud, 281, SBAR_Y(189), ACCESS_ICON, 0, 0, 1);

    /* weapon_amounts(p, 96, 182): N:ammo/max, the held weapon brighter */
    int cw = pl.curr_weapon, x = 96, y = 182;
#define GOT(w)  ((pl.gotweapon & (1 << (w))) != 0)
    hud_weaponnum(hud, PISTOL_WEAPON, x, y, pl.ammo[PISTOL_WEAPON], weapon_maxammo(PISTOL_WEAPON),
                  12 - 20 * (cw == PISTOL_WEAPON), 1);
    hud_weaponnum(hud, SHOTGUN_WEAPON, x, y + 6, pl.ammo[SHOTGUN_WEAPON], weapon_maxammo(SHOTGUN_WEAPON),
                  (!GOT(SHOTGUN_WEAPON)) * 9 + 12 - 18 * (cw == SHOTGUN_WEAPON), 1);
    hud_weaponnum(hud, CHAINGUN_WEAPON, x, y + 12, pl.ammo[CHAINGUN_WEAPON], weapon_maxammo(CHAINGUN_WEAPON),
                  (!GOT(CHAINGUN_WEAPON)) * 9 + 12 - 18 * (cw == CHAINGUN_WEAPON), 1);
    hud_weaponnum(hud, RPG_WEAPON, x + 39, y, pl.ammo[RPG_WEAPON], weapon_maxammo(RPG_WEAPON),
                  (!GOT(RPG_WEAPON)) * 9 + 12 - 19 * (cw == RPG_WEAPON), 0);
    hud_weaponnum(hud, HANDBOMB_WEAPON, x + 39, y + 6, pl.ammo[HANDBOMB_WEAPON], weapon_maxammo(HANDBOMB_WEAPON),
                  ((!pl.ammo[HANDBOMB_WEAPON]) | (!GOT(HANDBOMB_WEAPON))) * 9 + 12 -
                  19 * (cw == HANDBOMB_WEAPON || cw == HANDREMOTE_WEAPON), 0);
    hud_weaponnum(hud, SHRINKER_WEAPON, x + 39, y + 12, pl.ammo[SHRINKER_WEAPON], weapon_maxammo(SHRINKER_WEAPON),
                  (!GOT(SHRINKER_WEAPON)) * 9 + 12 - 18 * (cw == SHRINKER_WEAPON || cw == GROW_WEAPON), 0);
    hud_weaponnum(hud, DEVISTATOR_WEAPON, x + 70, y, pl.ammo[DEVISTATOR_WEAPON], weapon_maxammo(DEVISTATOR_WEAPON),
                  (!GOT(DEVISTATOR_WEAPON)) * 9 + 12 - 18 * (cw == DEVISTATOR_WEAPON), 0);
    hud_weaponnum(hud, TRIPBOMB_WEAPON, x + 70, y + 6, pl.ammo[TRIPBOMB_WEAPON], weapon_maxammo(TRIPBOMB_WEAPON),
                  (!GOT(TRIPBOMB_WEAPON)) * 9 + 12 - 18 * (cw == TRIPBOMB_WEAPON), 0);
    hud_weaponnum(hud, -1, x + 70, y + 12, pl.ammo[FREEZE_WEAPON], weapon_maxammo(FREEZE_WEAPON),
                  (!GOT(FREEZE_WEAPON)) * 9 + 12 - 18 * (cw == FREEZE_WEAPON), 0);
#undef GOT

    /* health, armour, current ammo */
    hud_digital(hud, 32, 200 - 17, pl.health);
    hud_digital(hud, 64, 200 - 17, pl.armor);
    if (cw != KNEE_WEAPON) {
        int w = (cw == HANDREMOTE_WEAPON) ? HANDBOMB_WEAPON : cw;
        hud_digital(hud, 230 - 22, 200 - 17, pl.ammo[w]);
    }

    /* inventory: icon, ON/OFF/AUTO, percentage */
    int ic = pl.inven_icon;
    if (ic >= 0 && ic < INV_NUM && pl.inv[ic] > 0) {
        hud_pic(hud, 231, SBAR_Y(200 - 21), inv_icons[ic], 0, 0, 1);
        hud_minitext(hud, 292 - 30, 190, "%", 6);
        if (ic >= INV_SCUBA) hud_minitext(hud, 284 - 35, 180, "AUTO", 2);
        int on = -1;
        if (ic == INV_HOLODUKE) on = (pl.holoduke_on > 0 && pl.holoduke_on < MAXSPRITES &&
                                      sprite[pl.holoduke_on].statnum == STAT_DUMMYPLAYER);
        if (ic == INV_JETPACK) on = pl.jetpack_on != 0;
        if (ic == INV_HEATS) on = pl.heat_on != 0;
        if (on > 0) hud_minitext(hud, 288 - 30, 180, "ON", 0);
        else if (on == 0) hud_minitext(hud, 284 - 30, 180, "OFF", 2);
        int pct = 0;
        switch (ic) {
        case INV_FIRSTAID: pct = pl.inv[INV_FIRSTAID]; break;
        case INV_STEROIDS: pct = (pl.inv[INV_STEROIDS] + 3) >> 2; break;
        case INV_HOLODUKE: pct = (pl.inv[INV_HOLODUKE] + 15) / 24; break;
        case INV_JETPACK:  pct = (pl.inv[INV_JETPACK] + 15) >> 4; break;
        case INV_HEATS:    pct = pl.inv[INV_HEATS] / 12; break;
        case INV_SCUBA:    pct = (pl.inv[INV_SCUBA] + 63) >> 6; break;
        case INV_BOOTS:    pct = pl.inv[INV_BOOTS] >> 1; break;
        }
        static const int8_t inv_xs[3][3] = { { 4 }, { 0, 4 }, { -4, 0, 4 } };   /* GAME.C invennum() */
        hud_3x5_number(hud, pct, inv_xs, 284 - 30, 200 - 6, 0, 0);
    }
}

/* ---------------------------------------------------- weapon display */
/* PLAYER.C displayweapon(): Duke's 320x200 weapon coordinates.  Duke's
 * rotatesprite (flag 2) places weapons relative to the view window: with
 * the 34-line status bar the window is lines 0..165 (centre 83.5), so a
 * weapon at y lands on line y - 16.5 and is clipped at line 166.  Our view
 * ends at line 192, so y + (192 - 166 - 16.5) ~ y + 10 shows exactly the
 * part of each weapon Duke shows above its bar.  The bob only moves a
 * weapon between its rest height and 16 px above it, and every weapon's
 * bottom edge stays at least 19 px below the bar even at the top of it. */
#define WEAPON_YSHIFT 10

/* The 32X ignores byte writes of palette index 0, even through the normal
 * framebuffer address. Pick a nonzero equivalent of the current colour 0 so
 * native-resolution sprite pixels can still overwrite the background. */
static uint8_t weapon_opaque_zero(void)
{
    const uint16_t *cram = shown_pal ? shown_pal : cur_cram;
    uint16_t z = cram[0];
    int zr = z & 31, zg = (z >> 5) & 31, zb = (z >> 10) & 31;
    int best = 1, bestdist = 0x7fffffff;
    for (int i = 1; i < 255; i++) {
        uint16_t c = cram[i];
        int r = (c & 31) - zr, g = ((c >> 5) & 31) - zg, b = ((c >> 10) & 31) - zb;
        int d = r*r + g*g + b*b;
        if (d < bestdist) { bestdist = d; best = i; if (!d) break; }
    }
    return (uint8_t)best;
}

static void weapon_pic(int x, int y, int pic, int shade, int pal, int flip)
{
    if (!tile_present(pic)) return;
    const dk_tile_t *t = &tiledir[pic];
    int xo = (t->ow >> 1) + t->xoff, yo = (t->oh >> 1) + t->yoff;
    int left = flip ? x - (t->ow - xo) : x - xo;      /* myospal orientation 4: mirrored */
    int top = y - yo + WEAPON_YSHIFT;
    if (pic >= CHAINGUN && pic <= CHAINGUN + 6 && t->shift == 0) {
        /* The chaingun is made of overlapping, finely detailed tiles. Drawing
         * their original pixels at 320-wide avoids losing thin lines at 160x96
         * and keeps the barrel/body seam aligned with the original art. */
        const uint8_t *px = (const uint8_t *)DKPTR(t->off);
        const uint8_t *lut = palookup[pal & 31] + (max(0, min(31, shade)) << 8);
        const uint8_t opaque_zero = weapon_opaque_zero();
        for (int sy = 0; sy < t->h; sy++) {
            int dy = top + sy;
            if ((unsigned)dy >= (unsigned)(rview.ydim * 2)) continue;
            uint8_t *line = plat_text_line(dy);
            if (!line) continue;
            for (int sx = 0; sx < t->w; sx++) {
                int dx = left + sx;
                if ((unsigned)dx >= (unsigned)(rview.xdim * 2)) continue;
                uint8_t c = px[(flip ? t->w - 1 - sx : sx) * t->h + sy];
                if (c != 255) {
                    uint8_t shaded = lut[c];
                    line[dx] = shaded ? shaded : opaque_zero;
                }
            }
        }
    } else blit_view(pic, left >> 1, top >> 1, shade, pal, flip);
}

static void draw_weapon(void)
{
    if (pl.dead_flag) return;
    int cw = pl.curr_weapon, kb = pl.kickback_pic;
    int gs = sector[pl.cursectnum].floorshade;
    if (gs > 24) gs = 24;
    int pal = sector[pl.cursectnum].floorpal;
    int gun_pos = 80 - (pl.weapon_pos * pl.weapon_pos);
    int xo = (160 - 90) - (sintable[((pl.weapon_sway >> 1) + 512) & 2047] / (1024 + 512)) - 58;
    if (sprite[pl.i].xrepeat < 32)
        gun_pos -= klabs(sintable[(pl.weapon_sway << 2) & 2047] >> 9);
    else gun_pos -= klabs(sintable[(pl.weapon_sway >> 1) & 2047] >> 10);
    gun_pos -= (pl.hard_landing << 3);
    int rate = weapon_rate(cw);

    switch (cw) {
    case KNEE_WEAPON:
        if (kb > 0) {
            int k = (kb * 14) / (rate > 0 ? rate : 1);          /* Duke's 14-step kick */
            if (k < 5 || k > 9) weapon_pic(xo + 220, 250 - gun_pos, KNEE, gs, pal, 0);
            else weapon_pic(xo + 160, 214 - gun_pos, KNEE + 1, gs, pal, 0);
        }
        break;
    case PISTOL_WEAPON:
        if (pl.reloading > 0) {                     /* Duke kickback 6..25 = the reload */
            int k = 26 - pl.reloading;
            if (k < 10) weapon_pic(194, 230 - gun_pos, FIRSTGUN + 4, gs, pal, 0);
            else if (k < 15) {
                weapon_pic(244 - (k << 3), 130 - gun_pos + (k << 4), FIRSTGUN + 6, gs, pal, 0);
                weapon_pic(224, 220 - gun_pos, FIRSTGUN + 5, gs, pal, 0);
            } else if (k < 20) {
                weapon_pic(124 + (k << 1), 430 - gun_pos - (k << 3), FIRSTGUN + 6, gs, pal, 0);
                weapon_pic(224, 220 - gun_pos, FIRSTGUN + 5, gs, pal, 0);
            } else if (k < 23) {
                weapon_pic(184, 235 - gun_pos, FIRSTGUN + 8, gs, pal, 0);
                weapon_pic(224, 210 - gun_pos, FIRSTGUN + 5, gs, pal, 0);
            } else if (k < 25) {
                weapon_pic(164, 245 - gun_pos, FIRSTGUN + 8, gs, pal, 0);
                weapon_pic(224, 220 - gun_pos, FIRSTGUN + 5, gs, pal, 0);
            } else weapon_pic(194, 235 - gun_pos, FIRSTGUN + 5, gs, pal, 0);
        } else {
            static const int8_t kb_frames[5] = { 0, 1, 2, 0, 0 };
            int k = kb < 5 ? kb : 0;
            int l = 195 - 12 + xo;
            if (k == 2) l -= 3;
            weapon_pic(l, 244 - gun_pos, FIRSTGUN + kb_frames[k], gs, pal, 0);
        }
        break;
    case SHOTGUN_WEAPON: {
        int k = kb ? 1 + ((kb - 1) * 30) / (rate > 1 ? rate : 1) : 0;    /* map onto Duke's 30 */
        xo -= 8;
        switch (k) {
        case 1: case 2:
            weapon_pic(xo + 168, 201 - gun_pos, SHOTGUN + 2, -128, pal, 0);
            /* fall through */
        case 0: case 6: case 7: case 8:
            weapon_pic(xo + 146, 202 - gun_pos, SHOTGUN, gs, pal, 0);
            break;
        case 3: case 4: case 5: case 9: case 10: case 11: case 12:
            if (k > 1 && k < 5) {
                gun_pos -= 40; xo += 20;
                weapon_pic(xo + 178, 194 - gun_pos, SHOTGUN + 1 + ((k - 1) >> 1), -128, pal, 0);
            }
            weapon_pic(xo + 158, 220 - gun_pos, SHOTGUN + 3, gs, pal, 0);
            break;
        case 13: case 14: case 15:
            weapon_pic(32 + xo + 166, 210 - gun_pos, SHOTGUN + 4, gs, pal, 0); break;
        case 16: case 17: case 18: case 19: case 24: case 25: case 26: case 27:
            weapon_pic(64 + xo + 170, 196 - gun_pos, SHOTGUN + 5, gs, pal, 0); break;
        case 20: case 21: case 22: case 23:
            weapon_pic(64 + xo + 176, 196 - gun_pos, SHOTGUN + 6, gs, pal, 0); break;
        default:
            weapon_pic(32 + xo + 156, 206 - gun_pos, SHOTGUN + 4, gs, pal, 0); break;
        }
        break;
    }
    case CHAINGUN_WEAPON:
        if (kb > 0) { gun_pos -= sintable[(kb << 7) & 2047] >> 12; xo += 1 - (krand() & 3); }
        weapon_pic(xo + 168, 260 - gun_pos, CHAINGUN, gs, pal, 0);
        if (kb > 0) {
            int i = krand() & 7;
            weapon_pic(i + xo - 4 + 162, i - (kb >> 1) + 208 - gun_pos, CHAINGUN + 5 + (kb > 1), gs, pal, 0);
            weapon_pic(xo + 178, 233 - gun_pos, CHAINGUN + 1 + (kb & 3), gs, pal, 0);
        } else weapon_pic(xo + 178, 233 - gun_pos, CHAINGUN + 1, gs, pal, 0);
        break;
    case RPG_WEAPON:
        xo -= sintable[(768 + (kb << 7)) & 2047] >> 11;
        gun_pos += sintable[(768 + (kb << 7)) & 2047] >> 11;
        if (kb > 0 && kb < 8) weapon_pic(xo + 164, 176 - gun_pos, RPGGUN + (kb >> 1), gs, pal, 0);
        weapon_pic(xo + 164, 176 - gun_pos, RPGGUN, gs, pal, 0);
        break;
    case HANDBOMB_WEAPON:
        weapon_pic(xo + 190, 260 - gun_pos, HANDTHROW, gs, pal, 0);
        break;
    case HANDREMOTE_WEAPON: {
        static const int8_t remote_frames[11] = { 0, 1, 1, 2, 1, 1, 0, 0, 0, 0, 0 };
        weapon_pic(-48 + 150, 258 - gun_pos, HANDREMOTE + remote_frames[kb < 11 ? kb : 0], gs, pal, 0);
        break;
    }
    case DEVISTATOR_WEAPON:
        if (kb) {
            static const int8_t cycloidy[7] = { 0, 4, 12, 24, 12, 4, 0 };
            int c = cycloidy[kb < 7 ? kb : 0], i = kb >> 2 ? 1 : 0;
            if (gametic & 1) {
                weapon_pic((c >> 1) + xo + 268, c + 238 - gun_pos, DEVISTATOR + i, -32, pal, 0);
                weapon_pic(xo + 30, 240 - gun_pos, DEVISTATOR, gs, pal, 1);
            } else {
                weapon_pic(-(c >> 1) + xo + 30, c + 240 - gun_pos, DEVISTATOR + i, -32, pal, 1);
                weapon_pic(xo + 268, 238 - gun_pos, DEVISTATOR, gs, pal, 0);
            }
        } else {
            weapon_pic(xo + 268, 238 - gun_pos, DEVISTATOR, gs, pal, 0);
            weapon_pic(xo + 30, 240 - gun_pos, DEVISTATOR, gs, pal, 1);
        }
        break;
    case FREEZE_WEAPON:
        if (kb) {
            static const int8_t cat_frames[6] = { 0, 0, 1, 1, 2, 2 };
            int jx = krand() & 3;
            gun_pos -= 16;
            weapon_pic(xo + jx + 210, 261 - gun_pos, FREEZE + 2, -32, pal, 0);
            weapon_pic(xo + jx + 210, 235 - gun_pos, FREEZE + 3 + cat_frames[kb % 6], -32, pal, 0);
        } else weapon_pic(xo + 210, 261 - gun_pos, FREEZE, gs, pal, 0);
        break;
    case SHRINKER_WEAPON: case GROW_WEAPON:
        xo += 28;
        if (kb == 0) {
            weapon_pic(xo + 184, 18 + 240 - gun_pos, SHRINKER + 2, 16 - (sintable[(gametic << 6) & 2047] >> 10), 0, 0);
            weapon_pic(xo + 188, 18 + 240 - gun_pos, SHRINKER, gs, pal, 0);
        } else {
            int jx = krand() & 3;
            weapon_pic(xo + jx + 184, 18 + 240 - gun_pos, SHRINKER + 3 + (kb & 3), -32, 0, 0);
            weapon_pic(xo + jx + 188, 18 + 240 - gun_pos, SHRINKER + 1, gs, pal, 0);
        }
        break;
    case TRIPBOMB_WEAPON: {
        int la = 0, k = kb;
        xo += 8; gun_pos -= 10;
        if (k > 6) la += (k << 3);
        else if (k < 4) weapon_pic(xo + 142, la + 234 - gun_pos, HANDHOLDINGLASER + 3, gs, pal, 0);
        weapon_pic(xo + 130, la + 249 - gun_pos, HANDHOLDINGLASER + (k >> 2), gs, pal, 0);
        weapon_pic(xo + 152, la + 249 - gun_pos, HANDHOLDINGLASER + (k >> 2), gs, pal, 1);
        break;
    }
    }
}

static void draw_view_overlays(void)
{
    /* crosshair */
    int cx = rview.xdim >> 1, cy = (rview.ydim >> 1) + 1;
    rview.frame[cy * rview.pitch + cx] = dblpix(251);
    rview.frame[(cy - 2) * rview.pitch + cx] = dblpix(251);
    rview.frame[(cy + 2) * rview.pitch + cx] = dblpix(251);
    if (camsprite < 0 && !pl.dead_flag && pl.curr_weapon == CHAINGUN_WEAPON) {
        /* Native weapon rows may already have been copied out of the view. */
        for (int k = -2; k <= 2; k += 2)
            for (int row = 0; row < 2; row++) {
                uint8_t *line = plat_text_line((cy + k) * 2 + row);
                if (line) { line[cx * 2] = 251; line[cx * 2 + 1] = 251; }
            }
    }
    /* GAME.C operatefta(): quotes centred along the top of the view */
    if (pl.quote_time > 0 && quote_text) tfont_center_wrap(3, quote_text, TF_WHITE, 312);
    if (pl.dead_flag) tfont_center(SCREEN_VIEW_LINES / 2 - 14, "PRESS START TO RESTART", TF_WHITE);
}

/* -------------------------------------------------------- input */
static void build_sync(void)
{
    uint32_t p = pad_now, edge = pad_now & ~pad_prev;
    int six = (p & BTN_SIX) != 0;
    int running = 1;              /* console port: always-run like most Duke console versions */
    int32_t vel = 0, svel = 0, avel = 0;
    int keymove = running ? 80 : 40;
    int strafe = (p & BTN_A) && !six;           /* 3-button: hold A to strafe/look */
    sync1.bits = 0;
    if (strafe) {
        if (p & BTN_LEFT) svel += keymove;
        if (p & BTN_RIGHT) svel -= keymove;
        if (p & BTN_UP) sync1.bits |= SB_AIMUP;
        if (p & BTN_DOWN) sync1.bits |= SB_AIMDOWN;
    } else {
        if (p & (BTN_LEFT | BTN_RIGHT)) {
            turnheld += TICSPERFRAME;
            int amt = (turnheld >= 15) ? (running ? 30 : 15) : 5;
            if (p & BTN_LEFT) avel -= amt;
            if (p & BTN_RIGHT) avel += amt;
        } else turnheld = 0;
        if (p & BTN_UP) vel += keymove;
        if (p & BTN_DOWN) vel -= keymove;
    }
    if (six) {                                  /* 6-button extras */
        if (p & BTN_X) { if (p & BTN_LEFT) { svel += keymove; avel = 0; } if (p & BTN_RIGHT) { svel -= keymove; avel = 0; } }
        if (p & BTN_Y) sync1.bits |= SB_CROUCH;
        if (edge & BTN_Z) sync1.bits |= SB_NEXTWEAP;
        if (edge & BTN_MODE) sync1.bits |= SB_INVUSE;
        if (p & BTN_A) sync1.bits |= SB_JUMP;
    } else {
        /* 3-button: A tap = jump (when no direction held) */
        if ((p & BTN_A) && !(p & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT))) sync1.bits |= SB_JUMP;
    }
    if (p & BTN_B) sync1.bits |= SB_FIRE;
    if ((p & BTN_B) && (p & BTN_C)) sync1.bits |= SB_ALTFIRE;
    if (edge & BTN_C) sync1.bits |= SB_USE;
    if (p & BTN_C && (p & BTN_DOWN) && !six) sync1.bits |= SB_CROUCH;
    sync1.bits |= SB_RUN;
    /* Duke getinput: rotate (vel, svel) into world space */
    sync1.fvel = mulscale9(vel, sintable[(pl.ang + 2560) & 2047]) + mulscale9(svel, sintable[(pl.ang + 2048) & 2047]);
    sync1.svel = mulscale9(vel, sintable[(pl.ang + 2048) & 2047]) + mulscale9(svel, sintable[(pl.ang + 1536) & 2047]);
    sync1.avel = avel;
}

/* MENUES.C probe(): Duke's spinning nukes either side of the selected item
 * (a text line of width w centred on the screen, top at y) */
static void menu_cursor(int y, int w)
{
    int f = (int)(plat_ticks() >> 2);                 /* ~15 frames/s at 60 Hz */
    int cy = y + (DFONT_HEIGHT >> 1);
    int pics[2] = { SPINNINGNUKEICON + 6 - ((6 + f) % 7), SPINNINGNUKEICON + (f % 7) };
    int cxs[2] = { (320 - w) / 2 - 18, (320 + w) / 2 + 17 };
    for (int k = 0; k < 2; k++) {
        if (!tile_present(pics[k])) continue;
        const dk_tile_t *t = &tiledir[pics[k]];      /* centred like rotatesprite */
        txt_pic(cxs[k] - (t->ow >> 1) - t->xoff, cy - (t->oh >> 1) - t->yoff, pics[k]);
    }
}

#define PAUSE_Y0 64
#define PAUSE_DY 22

/* START menu (pause): weapon/inventory selection for 3-button pads */
static int pause_menu(void)
{
    int sel = 0, done = 0;
    static const char *items[] = { "RESUME", "NEXT WEAPON", "NEXT ITEM", "USE ITEM", "QUIT TO TITLE" };
    uint32_t prev = pad_now;
    while (!done) {
        uint32_t p = plat_read_input(), e = p & ~prev;
        prev = p;
        if (e & BTN_UP) sel = (sel + 4) % 5;
        if (e & BTN_DOWN) sel = (sel + 1) % 5;
        if (e & (BTN_START | BTN_B | BTN_C)) {
            switch (sel) {
            case 0: done = 1; break;
            case 1: sync1.bits = SB_NEXTWEAP; done = 2; break;
            case 2: sync1.bits = SB_INVNEXT; done = 2; break;
            case 3: sync1.bits = SB_INVUSE; done = 2; break;
            case 4: return -1;
            }
        }
        plat_frame_begin();
        plat_render_view();
        {   /* dim the frozen view so the menu stands out (Duke's shade tables) */
            const uint8_t *lut = palookup[0] + (18 << 8);
            for (int y = 0; y < rview.ydim; y++)
                for (int x = 0; x < rview.xdim; x++) {
                    pixel_t *q = &rview.frame[y * rview.pitch + x];
                    *q = dblpix(lut[*q & 0xff]);
                }
        }
        dfont_center(28, "PAUSED", dk_cram);
        for (int k = 0; k < 5; k++)
            dfont_center(PAUSE_Y0 + k * PAUSE_DY, items[k], dk_cram);
        menu_cursor(PAUSE_Y0 + sel * PAUSE_DY, dfont_width(items[sel]));
        hud_dirty = 2; draw_hud();
        plat_flip();
    }
    pad_prev = pad_now = plat_read_input();
    return done;
}

/* ------------------------------------------------------ level setup */
static void setup_sky(void)
{
    int sky = -1;
    for (int s = 0; s < numsectors; s++) if (sector[s].ceilingstat & 1) { sky = sector[s].ceilingpicnum; break; }
    memset(rview.pskyoff, 0, 8);
    rview.parallaxyscale = 32768;
    switch (sky) {
    case CLOUDYOCEAN: rview.parallaxyscale = 65536; break;
    case MOONSKY1: rview.pskyoff[6] = 1; rview.pskyoff[1] = 2; rview.pskyoff[4] = 2; rview.pskyoff[2] = 3; break;
    case BIGORBIT1: rview.pskyoff[5] = 1; rview.pskyoff[6] = 2; rview.pskyoff[7] = 3; rview.pskyoff[2] = 4; break;
    case LA: {
        static const int8_t la[8] = { 1, 2, 1, 3, 4, 0, 2, 3 };
        memcpy(rview.pskyoff, la, 8);
        rview.parallaxyscale = 16384 + 1024;
        break;
    }
    }
}

static player_t saved_pl;

void game_level_start(int idx)
{
    int32_t px, py, pz; int16_t pang, psect;
    sound_stopall();
    engine_loadboard(idx, &px, &py, &pz, &pang, &psect);
    memset(act, 0, sizeof(act));
    sector_init_level();
    actors_spawn_level();
    align_warp_elevators();         /* GAME.C enterlevel(): after prelevel() */
    player_spawn_at(px, py, pz, pang, psect);
    /* Toxic Dump starts with a full air tank, whether selected directly or
     * reached from Death Row.  saved_pl below also preserves it on retry. */
    if (idx == 3) {
        pl.inv[INV_SCUBA] = CON_SCUBA_AMOUNT;
        pl.inven_icon = INV_SCUBA;
        if (!(pl.gotweapon & (1 << SHOTGUN_WEAPON)))
            pl.wantweapon = SHOTGUN_WEAPON;
        pl.gotweapon |= (1 << SHOTGUN_WEAPON);
        if (pl.ammo[SHOTGUN_WEAPON] < 10) pl.ammo[SHOTGUN_WEAPON] = 10;
    }
    setup_sky();
    level_index = idx;
    gametic = 0;
    totalclock = 0;
    hud_dirty = 2;
    cur_palmode = -1;
    const dk_map_t *mi = engine_mapinfo(idx);
    player_quote_text(mi ? mi->title : "");
    saved_pl = pl;
}

static void game_tick(void)
{
    build_sync();
    player_process();
    actors_move();
    moveeffectors();
    doanimations();
    if ((gametic & 7) == 0) animatewalls();
    if ((gametic & 15) == 0) sound_ambient_update();
    gametic++;
    totalclock += TICSPERFRAME;
}

static void render_game(void)
{
    rview.posx = pl.posx; rview.posy = pl.posy; rview.posz = pl.posz;
    rview.ang = pl.ang;
    rview.horiz = pl.horiz + pl.horizoff;
    rview.sectnum = pl.cursectnum;
    rview.visibility = visibility_g;
    rview.clock = totalclock;
    rview.viewsprite = pl.i;
    rview.flags = pl.heat_on ? RVF_NIGHTVISION : 0;
    if (earthquaketime > 0) { rview.posx += (krand() & 127) - 64; rview.posz += (krand() & 511) - 256; }
    if (camsprite >= 0 && sprite[camsprite].statnum < MAXSTATUS) {   /* security camera view */
        spritetype *c = &sprite[camsprite];
        rview.posx = c->x; rview.posy = c->y; rview.posz = c->z;
        rview.ang = c->ang + ((sintable[(gametic << 3) & 2047] * c->hitag) >> 14);
        rview.sectnum = c->sectnum; rview.horiz = 100;
        if (sync1.fvel || sync1.svel || (pad_now & (BTN_B | BTN_C))) camsprite = -1;
    }
    if (!shown_pal) update_palette();      /* else: after the first flip (play_level) */
    txt_palette(0);
    plat_frame_begin();
    plat_render_view();
    if (camsprite < 0) draw_weapon();
    draw_view_overlays();
    draw_hud();
}

static int play_level(void)
{
    uint32_t last = plat_ticks();
    int32_t acc = 0;
    int pal = plat_pal();
    int deadwait = 0;
    uint32_t frames = 0;
    pad_prev = pad_now = plat_read_input();
    for (;;) {
        uint32_t now = plat_ticks();
        acc += (int32_t)(now - last) * 30;
        last = now;
        int n = 0;
        int rate = pal ? 50 : 60;
        while (acc >= rate && n < 4) {
            pad_prev = pad_now;
            pad_now = plat_read_input();
            if ((pad_now & ~pad_prev) & BTN_START) {
                if (pl.dead_flag && deadwait > 30) return 0;
                if (!pl.dead_flag) { int r = pause_menu(); if (r < 0) return -1; last = plat_ticks(); acc = 0; if (r == 2) { player_process(); } }
            }
            game_tick();
            acc -= rate;
            n++;
            if (pl.dead_flag) deadwait++;
            if (pl.level_done && --pl.timebeforeexit <= 0) return 1;
        }
        if (n == 0 && acc < rate) { /* nothing to do yet: still render to keep animations smooth */ }
        render_game();
        plat_flip();
        if (shown_pal) { cur_palmode = -1; update_palette(); }   /* picture palette out */
        frames++;
        plat_telemetry((uint16_t)(0x3000 | (level_index << 8) | (pl.health & 0xff)), (uint16_t)frames);
    }
}

/* ----------------------------------------------------- screens */
/* full-screen art in the view area, black status strip; text is drawn by
 * the caller afterwards (text must come last: it copies the view lines) */
static void draw_screen_tile(int pic)
{
    plat_frame_begin();
    rview.xdim = RMAXXDIM; rview.ydim = 96;
    rview.frame = plat_view_base(); rview.pitch = 160;
    for (int y = 0; y < rview.ydim; y++) for (int x = 0; x < rview.xdim; x++) rview.frame[y * rview.pitch + x] = 0;
    if (tile_present(pic)) {
        const dk_tile_t *t = &tiledir[pic];
        int w = t->shift ? t->w : t->w / 2, h = t->shift ? t->h : t->h / 2;
        blit_view(pic, (rview.xdim - w) / 2, (rview.ydim - h) / 2, 0, 0, 0);
    }
    uint8_t *hud = (uint8_t *)plat_hud_base();
    memset(hud, 0, 320 * HUD_LINES_G);
    txt_palette(0);
}

static uint32_t wait_press(uint32_t mask)
{
    uint32_t prev = plat_read_input();
    for (;;) {
        uint32_t p = plat_read_input();
        uint32_t e = p & ~prev;
        prev = p;
        if (e & mask) return e;
        plat_flip();
    }
}

static int start_map;          /* first level of the run (skill menu LEFT/RIGHT) */

/* Full-screen pictures from assets/ (title, level transition): convert.py
 * stores the 320x224 pixels and each picture's own palette in the blob; they
 * are copied to native-resolution lines through plat_text_line(). */
static const uint16_t *pic_palette(int n)
{
    const dk_pic_t *p = engine_pic(n);
    return p ? (const uint16_t *)DKPTR(p->off_cram) : dk_cram;
}

static int draw_picture(int n)
{
    const dk_pic_t *p = engine_pic(n);
    plat_frame_begin();
    if (p && p->lz[0] && p->w == 320 && p->h == 224) {
        /* LZ4 copies from its own earlier output, and the 32X framebuffer
         * can't be read back reliably, so decode in RAM first.  The map's
         * wall array is free whenever these screens are up (power-on,
         * title, ENTERING, LEVEL COMPLETED: no level in play; the next one
         * is loaded afresh) and is big enough for a whole screen. */
        _Static_assert(sizeof(wall) >= 320 * 224, "wall[] too small for a screen");
        uint8_t *tmp = (uint8_t *)wall;
        const uint8_t *src = (const uint8_t *)DKPTR(p->off_px);
        lz4_decode(src, p->lz[0], tmp, 192 * 320);
        lz4_decode(src + p->lz[0], p->lz[1], tmp + 192 * 320, 32 * 320);
        for (int y = 0; y < 224; y++) memcpy(plat_screen_line(y), tmp + y * 320, 320);
    } else {
        for (int y = 0; y < 224; y++) {
            uint8_t *line = plat_screen_line(y);
            if (!line) continue;
            if (p && y < p->h && p->w >= 320) memcpy(line, (const uint8_t *)DKPTR(p->off_px) + y * p->w, 320);
            else memset(line, 0, 320);
        }
    }
    txt_palette(pic_palette(n));         /* text on it uses its palette */
    return p != 0;
}

/* Show the frame just drawn, then load `pal` if it isn't in CRAM yet: the
 * write happens in the same vblank as the flip, so no frame is ever shown
 * with another screen's colours. */
static void flip_with_palette(const uint16_t *pal)
{
    plat_flip();
    if (pal != shown_pal) { plat_set_palette(pal); shown_pal = pal; cur_palmode = -1; }
}

/* ------------------------------------------------ power-on and title */
int game_skip_intro;                     /* desktop test runner: no intro */

static int vblank_rate(void) { return plat_pal() ? 50 : 60; }

static int start_edge(uint32_t *prev);

/* fade `pal` from level `from` to `to` (0 = black .. 32 = full) over
 * `frames` vblanks; the screen shows the same picture in both buffers.
 * With `prev`, a START press during the fade is reported (returns 1). */
static int fade_palette(const uint16_t *pal, int from, int to, int frames, uint32_t *prev)
{
    int pressed = 0;
    if (frames < 1) frames = 1;
    for (int i = 1; i <= frames; i++) {
        if (prev && start_edge(prev)) pressed = 1;
        int lv = from + (to - from) * i / frames;
        for (int c = 0; c < 256; c++) {
            uint16_t v = pal[c];
            int r = ((v & 31) * lv) >> 5, g = (((v >> 5) & 31) * lv) >> 5, b = (((v >> 10) & 31) * lv) >> 5;
            cur_cram[c] = (uint16_t)(0x8000 | (b << 10) | (g << 5) | r);
        }
        plat_flip();
        plat_set_palette(cur_cram);
    }
    shown_pal = (to >= 32) ? pal : cur_cram;
    cur_palmode = -1;
    return pressed;
}

static void palette_black(void)
{
    for (int c = 0; c < 256; c++) cur_cram[c] = 0x8000;
    plat_set_palette(cur_cram);
    shown_pal = cur_cram;
    cur_palmode = -1;
}

static int start_edge(uint32_t *prev)
{
    uint32_t p = plat_read_input(), e = p & ~*prev;
    *prev = p;
    return (e & BTN_START) != 0;
}

/* GAME.C Logo(): the 3D Realms screen (assets/realms.png) at power-on - it
 * fades in, stays about 8 seconds (START skips it) and fades to black */
static void intro_realms(void)
{
    if (!engine_pic(DK_PIC_REALMS)) return;
    const uint16_t *pal = pic_palette(DK_PIC_REALMS);
    int rate = vblank_rate();
    plat_telemetry(0x0800, 0);
    palette_black();
    for (int f = 0; f < 2; f++) { draw_picture(DK_PIC_REALMS); plat_flip(); }
    uint32_t prev = plat_read_input(), t0 = plat_ticks();
    if (!fade_palette(pal, 0, 32, rate / 2, &prev))
        while ((int32_t)(plat_ticks() - t0) < 8 * rate && !start_edge(&prev)) plat_flip();
    fade_palette(pal, 32, 0, rate / 2, 0);
}

/* Title: assets/title.png, the DUKE NUKEM / 3D logo (assets/title_logo.png)
 * slamming in as in GAME.C Logo() - each part zooms up from nothing with a
 * PIPEBOMB_EXPLODE - then "PRESS START" (Duke's big font) and the warning */
#define TITLE_PRESS_Y 186
#define TITLE_WARN_Y  210

/* a logo part zoomed to s/65536 of its size, centred on its final place */
static void draw_logo_part(int n, int32_t s)
{
    const dk_pic_t *p = engine_pic(n);
    if (!p || s <= 0) return;
    if (s > 65536) s = 65536;
    int dw = (p->w * s) >> 16, dh = (p->h * s) >> 16;
    if (dw <= 0 || dh <= 0) return;
    int x0 = p->x + (p->w >> 1) - (dw >> 1), y0 = p->y + (p->h >> 1) - (dh >> 1);
    int32_t stepx = ((int32_t)p->w << 16) / dw, stepy = ((int32_t)p->h << 16) / dh;
    const uint8_t *px = (const uint8_t *)DKPTR(p->off_px);
    int32_t sy = 0;
    for (int r = 0; r < dh; r++, sy += stepy) {
        uint8_t *line = plat_text_line(y0 + r);
        if (!line) continue;
        const uint8_t *src = px + (sy >> 16) * p->w;
        int32_t sx = 0;
        for (int c = 0; c < dw; c++, sx += stepx) {
            uint8_t v = src[sx >> 16];
            if (v != DK_PIC_SEETHROUGH && (unsigned)(x0 + c) < 320u) line[x0 + c] = v;
        }
    }
}

/* the rows the logo covers, back to the plain background (an LZ4 title is
 * still unpacked in the wall-array scratch from draw_picture(DK_PIC_TITLE)) */
static void title_restore(int y0, int y1)
{
    const dk_pic_t *bg = engine_pic(DK_PIC_TITLE);
    const uint8_t *rows = !bg ? 0 : bg->lz[0] ? (const uint8_t *)wall : (const uint8_t *)DKPTR(bg->off_px);
    for (int y = y0; y < y1; y++) {
        uint8_t *line = plat_text_line(y);
        if (!line) continue;
        if (rows) memcpy(line, rows + y * 320, 320);
        else memset(line, 0, 320);
    }
}

static void title_texts(void)
{
    dfont_center(TITLE_PRESS_Y, "PRESS START", pic_palette(DK_PIC_TITLE));
    txt_palette(pic_palette(DK_PIC_TITLE));
    txt_center(TITLE_WARN_Y, "WARNING: ADULT CONTENT", TS_YELLOW);
}

static void draw_title(void)
{
    if (!draw_picture(DK_PIC_TITLE))     /* blob without a picture */
        txt_pic((320 - TILESIZX(DUKENUKEM)) / 2, 80, DUKENUKEM);
    draw_logo_part(DK_PIC_LOGO_DUKE, 65536);
    draw_logo_part(DK_PIC_LOGO_3D, 65536);
    draw_logo_part(DK_PIC_LOGO_32X, 65536);
    title_texts();
}

/* the animated version; returns early if START is pressed */
static void title_sequence(void)
{
    const uint16_t *pal = pic_palette(DK_PIC_TITLE);
    const dk_pic_t *a = engine_pic(DK_PIC_LOGO_DUKE), *b = engine_pic(DK_PIC_LOGO_3D);
    const dk_pic_t *c = engine_pic(DK_PIC_LOGO_32X);
    int ly0 = 224, ly1 = 0;
    if (a) { ly0 = a->y; ly1 = a->y + a->h; }
    if (b) { if (b->y < ly0) ly0 = b->y; if (b->y + b->h > ly1) ly1 = b->y + b->h; }
    if (c) { if (c->y < ly0) ly0 = c->y; if (c->y + c->h > ly1) ly1 = c->y + c->h; }
    int rate = vblank_rate();
    int t_duke = rate, d_duke = rate / 2;               /* totalclock 120 .. 180 */
    int t_3d = rate * 220 / 120, d_3d = rate / 4;       /* totalclock 220 .. 250 */
    int t_32x = t_3d + d_3d + rate / 6, d_32x = rate / 4;
    int t_text = (c ? t_32x + d_32x : t_3d + d_3d) + rate / 3;
    palette_black();
    for (int f = 0; f < 2; f++) { draw_picture(DK_PIC_TITLE); plat_flip(); }
    uint32_t prev = plat_read_input();
    if (fade_palette(pal, 0, 32, rate / 2, &prev)) return;
    uint32_t t0 = plat_ticks();
    int boom1 = 0, boom2 = 0, boom3 = 0;
    for (;;) {
        if (start_edge(&prev)) return;
        int t = (int)(plat_ticks() - t0);
        if (ly1 > ly0) title_restore(ly0, ly1);
        if (t >= t_duke) {
            if (!boom1) { globalsound(PIPEBOMB_EXPLODE); boom1 = 1; }
            draw_logo_part(DK_PIC_LOGO_DUKE, t >= t_duke + d_duke ? 65536 : ((t - t_duke) << 16) / d_duke);
        }
        if (t >= t_3d) {
            if (!boom2) { globalsound(PIPEBOMB_EXPLODE); boom2 = 1; }
            draw_logo_part(DK_PIC_LOGO_3D, t >= t_3d + d_3d ? 65536 : ((t - t_3d) << 16) / d_3d);
        }
        if (c && t >= t_32x) {
            if (!boom3) { globalsound(PIPEBOMB_EXPLODE); boom3 = 1; }
            draw_logo_part(DK_PIC_LOGO_32X, t >= t_32x + d_32x ? 65536 : ((t - t_32x) << 16) / d_32x);
        }
        if (t >= t_text) title_texts();
        plat_flip();
        if (t >= t_text + 4) break;                     /* both buffers complete */
    }
    while (!start_edge(&prev)) plat_flip();
}

/* Level transition picture (assets/transition.png): its dark band across
 * the middle carries the text, its dark bottom edge the tally */
#define TRANS_BAND_Y   104
#define TRANS_BOTTOM_Y 194
#ifdef HOSTGAME
#define ENTER_MIN_TICKS 0                /* desktop test runner: no hold */
#else
#define ENTER_MIN_TICKS 60               /* keep ENTERING up for about a second */
#endif

static void entering_screen(int idx)
{
    const dk_map_t *mi = engine_mapinfo(idx);
    for (int f = 0; f < 2; f++) {        /* both buffers */
        draw_picture(DK_PIC_TRANSITION);
        tfont_center(TRANS_BAND_Y, "ENTERING", TF_WHITE);
        if (mi) tfont_center(TRANS_BAND_Y + TFONT_LINE + 1, mi->title, TF_YELLOW);
        flip_with_palette(pic_palette(DK_PIC_TRANSITION));
    }
}

static int title_and_menu(void)
{
    /* power-on: the logo slams in; later visits show the finished title.
     * The picture's palette goes in right after the first flip, so no
     * frame shows with the wrong colours. */
    static int title_seen;
    plat_telemetry(0x1000, 0);
    if (!title_seen && !game_skip_intro && engine_pic(DK_PIC_TITLE)) {
        title_sequence();
    } else {
        for (int k = 0; k < 2; k++) {
            draw_title();
            flip_with_palette(pic_palette(DK_PIC_TITLE));
        }
        wait_press(BTN_START);
    }
    title_seen = 1;
    static const char *skills[4] = { "PIECE OF CAKE", "LET'S ROCK", "COME GET SOME", "DAMN I'M GOOD" };
    int sel = 1, nm = engine_nummaps(), drawn = 0;
    uint32_t prev = plat_read_input();
    start_map = 0;
    plat_telemetry(0x2000, (uint16_t)sel);
    for (;;) {
        uint32_t p = plat_read_input(), e = p & ~prev;
        prev = p;
        if (e & BTN_UP) sel = (sel + 3) & 3;
        if (e & BTN_DOWN) sel = (sel + 1) & 3;
        if (e & BTN_LEFT) start_map = (start_map + nm - 1) % nm;     /* starting level: the */
        if (e & BTN_RIGHT) start_map = (start_map + 1) % nm;         /* campaign runs on from it */
        if (e & (BTN_START | BTN_B | BTN_C)) break;
        draw_screen_tile(MENUSCREEN);
        tfont_center(32, "NEW GAME", TF_RED);
        tfont_center(52, "SELECT SKILL", TF_BLUE);
        for (int k = 0; k < 4; k++) tfont_center(70 + k * 16, skills[k], k == sel ? TF_YELLOW : TF_WHITE);
        {   /* "< E3L9  STADIUM >" */
            const dk_map_t *mi = engine_mapinfo(start_map);
            char line[48]; int n = 0;
            line[n++] = '<'; line[n++] = ' ';
            for (int c = 0; c < 8 && mi->name[c]; c++) line[n++] = mi->name[c];
            line[n++] = ' '; line[n++] = ' ';
            for (int c = 0; c < 32 && mi->title[c] && n < 44; c++) line[n++] = mi->title[c];
            line[n++] = ' '; line[n++] = '>'; line[n] = 0;
            tfont_center(140, "START LEVEL", TF_BLUE);
            tfont_center(156, line, start_map ? TF_YELLOW : TF_WHITE);
        }
        tfont_center(SCREEN_VIEW_LINES + 2, "UP/DOWN SKILL  LEFT/RIGHT LEVEL", TF_WHITE);
        tfont_center(SCREEN_VIEW_LINES + 2 + TFONT_LINE + 1, "PRESS START", TF_YELLOW);
        flip_with_palette(dk_cram);      /* title palette out */
        drawn = 1;
        plat_telemetry(0x2000, (uint16_t)(sel | (start_map << 8)));
    }
    if (!drawn) {                        /* never drew the menu: leave the title palette now */
        draw_screen_tile(MENUSCREEN);
        flip_with_palette(dk_cram);
    }
    globalsound(INTO_MENU);
    return sel + 1;          /* Duke player_skill 1..4 */
}

static void intermission(void)
{
    char l1[48], l2[48];
    int k = pl.actors_killed, mk = pl.max_actors_killed, s = pl.secret_rooms, ms = pl.max_secret_rooms;
    int t = pl.level_time / 30;
    int n = 0;
    memcpy(l1, "KILLS ", 6); n = 6;
    l1[n++] = (char)('0' + (k / 100) % 10); l1[n++] = (char)('0' + (k / 10) % 10); l1[n++] = (char)('0' + k % 10);
    l1[n++] = '/'; l1[n++] = (char)('0' + (mk / 100) % 10); l1[n++] = (char)('0' + (mk / 10) % 10); l1[n++] = (char)('0' + mk % 10);
    memcpy(l1 + n, "   SECRETS ", 11); n += 11;
    l1[n++] = (char)('0' + s % 10); l1[n++] = '/'; l1[n++] = (char)('0' + ms % 10); l1[n] = 0;
    memcpy(l2, "TIME ", 5); n = 5;
    l2[n++] = (char)('0' + (t / 600) % 10); l2[n++] = (char)('0' + (t / 60) % 10); l2[n++] = ':';
    l2[n++] = (char)('0' + (t % 60) / 10); l2[n++] = (char)('0' + t % 10); l2[n] = 0;
    const dk_map_t *mi = engine_mapinfo(level_index);
    globalsound(BONUS_SPEECH1 + (krand() & 3));
    for (int f = 0; f < 2; f++) {
        draw_picture(DK_PIC_TRANSITION);
        if (mi) tfont_center(TRANS_BAND_Y, mi->title, TF_WHITE);
        tfont_center(TRANS_BAND_Y + TFONT_LINE + 1, "LEVEL COMPLETED", TF_YELLOW);
        tfont_center(TRANS_BOTTOM_Y, l1, TF_WHITE);
        tfont_center(TRANS_BOTTOM_Y + TFONT_LINE, l2, TF_WHITE);
        flip_with_palette(pic_palette(DK_PIC_TRANSITION));
    }
    plat_telemetry(0x4000 | (level_index & 0xff), 0);
    wait_press(BTN_START | BTN_B | BTN_C);
}

void game_main(void)
{
    plat_set_palette(dk_cram);
    shown_pal = dk_cram;
    if (!game_skip_intro) intro_realms();
    for (;;) {
        game_skill = title_and_menu();
        player_init(1);
        int idx = start_map, nm = engine_nummaps();
        while (idx < nm) {
            uint32_t t0 = plat_ticks();
            entering_screen(idx);        /* stays up while the level loads */
            game_level_start(idx);
            while ((int32_t)(plat_ticks() - t0) < ENTER_MIN_TICKS) plat_flip();
            int r = play_level();
            if (r < 0) break;
            if (r == 0) { pl = saved_pl; continue; }       /* died: restart level */
            intermission();
            idx++;
            player_init(0);
        }
        if (idx >= nm) {
            for (int f = 0; f < 2; f++) {
                draw_screen_tile(DUKENUKEM);
                txt_center(SCREEN_VIEW_LINES + 6, "CAMPAIGN COMPLETE", TS_YELLOW);
                txt_center(SCREEN_VIEW_LINES + 18, "HAIL TO THE KING, BABY!", TS_RED);
                flip_with_palette(dk_cram);
            }
            wait_press(BTN_START);
        }
    }
}
