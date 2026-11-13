/*
 * render_impl.c - Build-style sector/portal renderer for the SH-2.
 *
 * Included twice (render_m.c, render_s.c) with RNAME() giving each SH-2 its
 * own copy of the code and state; each copy renders the column band
 * [cx0,cx1] of the same view.
 *
 * Algorithm (after Ken Silverman's Build engine, used under the Build
 * license): sector flood via scansector() into "bunches" of walls, closest
 * bunch first (bunchfront/wallfront), per-column clip window umost/dmost,
 * ceilings & floors drawn as row spans from the column spans, parallax
 * skies, then sprites and masked walls back-to-front clipped against the
 * saved per-wall windows (smost).  The texture mapping inner loops are new:
 * 16.16 coordinates with native 16-bit shifts and 2-byte doubled pixels.
 */
#include "render.h"

#ifndef RNAME
#error "define RNAME(x) before including render_impl.c"
#endif

#define MAXWALLSB 256
#define MAXBUNCHES 128
#define MAXSPRITESONSCREEN 96
#define MAXYSAVES (RMAXXDIM * 16)
#define MAXMASKWALLS 48
#define MAXSECTBORDER 96

/* ------------------------------------------------------------- state */
static int32_t cx0, cx1;
static int32_t posx, posy, posz, cosang, sinang;
static int32_t xdim, ydim, halfx, horizon, ysc, k16_6, globvis, pitch, fclock, nightvis;
static pixel_t *frame;
static int32_t viewsprite;
static int16_t cursect;

static int16_t umost[RMAXXDIM], dmost[RMAXXDIM];
static int16_t uplc[RMAXXDIM], dplc[RMAXXDIM], uwall[RMAXXDIM], dwall[RMAXXDIM];
static int32_t lwall[RMAXXDIM], dcol[RMAXXDIM];
static int16_t lastx[RMAXYDIM + 2];
static int16_t sky_done_y[RMAXXDIM];
static uint32_t sky_done_key;

static int32_t numscans, numbunches;
static int16_t thewall[MAXWALLSB], thesector[MAXWALLSB], xb1[MAXWALLSB], xb2[MAXWALLSB], p2[MAXWALLSB];
static int32_t yb1[MAXWALLSB], yb2[MAXWALLSB], st1[MAXWALLSB], st2[MAXWALLSB];
static int32_t rx1[MAXWALLSB], ry1[MAXWALLSB], rx2[MAXWALLSB], ry2[MAXWALLSB];
static int16_t bunchfirst[MAXBUNCHES], bunchlast[MAXBUNCHES];
static uint8_t bunchtemp[MAXBUNCHES];
static uint8_t gotsector[(MAXSECTORS + 7) >> 3];
static int16_t sectorborder[MAXSECTBORDER];
static int32_t numhits;

static int32_t spritesortcnt;
static int16_t tsprite[MAXSPRITESONSCREEN];
static int32_t spritesx[MAXSPRITESONSCREEN], spritesy[MAXSPRITESONSCREEN + 1];

static int32_t smostwallcnt, smostcnt, maskwallcnt;
static int16_t smostwall[MAXWALLSB], smoststart[MAXWALLSB];
static uint8_t smostwalltype[MAXWALLSB];
static uint8_t smost[MAXYSAVES];   /* umost/dmost copies (0..ydim, fits u8) */
static int16_t maskwall[MAXMASKWALLS];

rstats_t RNAME(rstats);
#if IS_32X && defined(RDEBUG)
#define RDBG(n) (*(volatile uint16_t *)0x2000402E = (uint16_t)(n))
#define RDBG2(n) (*(volatile uint16_t *)0x2000402C = (uint16_t)(n))
#elif defined(RHOSTDEBUG)
#include <stdio.h>
#define RDBG(n) fprintf(stderr, "RDBG %x\n", (unsigned)(n))
#define RDBG2(n) fprintf(stderr, "RDBG2 %x\n", (unsigned)(n))
#else
#define RDBG2(n) ((void)0)
#define RDBG(n) ((void)0)
#endif
#define STATS RNAME(rstats)

/* -------------------------------------------------------------- utils */
static inline int32_t shadeidx(int32_t shade, int32_t depth_yp, int32_t vis)
{
    if (nightvis) return 0;
    int32_t s = shade + (int32_t)(mul64(depth_yp, vis) >> 27);
    if (s < 0) s = 0;
    if (s > DK_NUMSHADES - 1) s = DK_NUMSHADES - 1;
    return s;
}

static inline int32_t sectvis(const sectortype *sec)
{
    if (sec->visibility != 0) return mulscale4(globvis, (int32_t)(uint8_t)(sec->visibility + 16));
    return globvis;
}

static inline const uint8_t *palptr(int pal, int s)
{
    return palookup[(unsigned)pal < DK_MAXPALS ? pal : 0] + (s << 8);
}

static int32_t animofs(int16_t tilenum, int16_t fakevar)
{
    int32_t i, k = 0, z, zz;
    int32_t pa = PICANM(tilenum);
    i = (fclock >> ((pa >> 24) & 15));
    if ((pa & 63) > 0) {
        switch (pa & 192) {
        case 64:
            k = (i % ((pa & 63) << 1));
            if (k >= (pa & 63)) k = ((pa & 63) << 1) - k;
            break;
        case 128:
            k = (i % ((pa & 63) + 1));
            break;
        case 192:
            k = -(i % ((pa & 63) + 1));
            break;
        }
    }
    (void)z; (void)zz; (void)fakevar;
    return k;
}

static inline int32_t picwrap(int32_t p)
{
    return ((uint32_t)p >= (uint32_t)numtiles) ? 0 : p;
}

/* t (16.16, 0..1) of the wall point seen through the centre of column x */
static int32_t wall_t_at(int z, int x)
{
    int32_t a1 = rx1[z] >> 4, b1 = ry1[z] >> 4, a2 = rx2[z] >> 4, b2 = ry2[z] >> 4;
    int32_t xc2 = 2 * x + 1 - xdim;
    int64_t num = (int64_t)xc2 * b1 - (int64_t)xdim * a1;
    int64_t den = (int64_t)xdim * (a2 - a1) - (int64_t)xc2 * (b2 - b1);
    if (den == 0) return 0;
    while (den > 0x3fffffff || den < -0x3fffffff) { den >>= 1; num >>= 1; }
    int32_t t = div64(num << 16, (int32_t)den);
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    return t;
}

/* ---------------------------------------------------------- scanning */
static void scansector(int16_t sectnum)
{
    walltype *wal, *wal2;
    int32_t xs, ys, x1, y1, x2, y2, xp1 = 0, yp1 = 0, xp2, yp2, templong;
    int32_t z, zz, startwall, endwall, numscansbefore, scanfirst, bunchfrst, nextsectnum;
    int32_t sectorbordercnt;

    if (sectnum < 0) return;
    sectorborder[0] = sectnum; sectorbordercnt = 1;
    do {
        sectnum = sectorborder[--sectorbordercnt];

        for (z = headspritesect[sectnum]; z >= 0; z = nextspritesect[z]) {
            spritetype *spr = &sprite[z];
            if ((spr->cstat & 0x8000) || spr->xrepeat == 0 || spr->yrepeat == 0 || z == viewsprite)
                continue;
            if (spritesortcnt >= MAXSPRITESONSCREEN) break;
            xs = spr->x - posx; ys = spr->y - posy;
            if ((spr->cstat & 48) || dmulscale6(xs, cosang, ys, sinang) > 0)
                tsprite[spritesortcnt++] = (int16_t)z;
        }

        gotsector[sectnum >> 3] |= pow2char[sectnum & 7];

        bunchfrst = numbunches;
        numscansbefore = numscans;
        startwall = sector[sectnum].wallptr;
        endwall = startwall + sector[sectnum].wallnum;
        scanfirst = numscans;
        for (z = startwall, wal = &wall[z]; z < endwall; z++, wal++) {
            nextsectnum = wal->nextsector;
            wal2 = &wall[wal->point2];
            x1 = wal->x - posx; y1 = wal->y - posy;
            x2 = wal2->x - posx; y2 = wal2->y - posy;

            if ((nextsectnum >= 0) && ((wal->cstat & 32) == 0))
                if ((gotsector[nextsectnum >> 3] & pow2char[nextsectnum & 7]) == 0) {
                    templong = x1 * y2 - x2 * y1;
                    if (((uint32_t)templong + 262144) < 524288)
                        if (mulscale5(templong, templong) <= (x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1))
                            if (sectorbordercnt < MAXSECTBORDER)
                                sectorborder[sectorbordercnt++] = (int16_t)nextsectnum;
                }

            if ((z == startwall) || (wall[z - 1].point2 != z)) {
                xp1 = dmulscale6(y1, cosang, -x1, sinang);
                yp1 = dmulscale6(x1, cosang, y1, sinang);
            } else {
                xp1 = xp2; yp1 = yp2;
            }
            xp2 = dmulscale6(y2, cosang, -x2, sinang);
            yp2 = dmulscale6(x2, cosang, y2, sinang);
            if ((yp1 < 256) && (yp2 < 256)) goto skipitaddwall;
            if (dmulscale32(xp1, yp2, -xp2, yp1) >= 0) goto skipitaddwall;
            if (numscans >= MAXWALLSB - 1) goto skipitaddwall;

            int32_t bx1, bx2, by1, by2;
            if (xp1 >= -yp1) {
                if ((xp1 > yp1) || (yp1 == 0)) goto skipitaddwall;
                bx1 = halfx + scale(xp1, halfx, yp1);
                if (xp1 >= 0) bx1++;
                if (bx1 >= xdim) bx1 = xdim - 1;
                by1 = yp1;
            } else {
                if (xp2 < -yp2) goto skipitaddwall;
                bx1 = 0;
                templong = yp1 - yp2 + xp1 - xp2;
                if (templong == 0) goto skipitaddwall;
                by1 = yp1 + scale(yp2 - yp1, xp1 + yp1, templong);
            }
            if (by1 < 256) goto skipitaddwall;

            if (xp2 <= yp2) {
                if ((xp2 < -yp2) || (yp2 == 0)) goto skipitaddwall;
                bx2 = halfx + scale(xp2, halfx, yp2) - 1;
                if (xp2 >= 0) bx2++;
                if (bx2 >= xdim) bx2 = xdim - 1;
                by2 = yp2;
            } else {
                if (xp1 > yp1) goto skipitaddwall;
                bx2 = xdim - 1;
                templong = xp2 - xp1 + yp1 - yp2;
                if (templong == 0) goto skipitaddwall;
                by2 = yp1 + scale(yp2 - yp1, yp1 - xp1, templong);
            }
            if ((by2 < 256) || (bx1 > bx2)) goto skipitaddwall;
            /* this CPU's column band */
            if (bx1 < cx0) bx1 = cx0;
            if (bx2 > cx1) bx2 = cx1;
            if (bx1 > bx2) goto skipitaddwall;

            thesector[numscans] = (int16_t)sectnum; thewall[numscans] = (int16_t)z;
            rx1[numscans] = xp1; ry1[numscans] = yp1;
            rx2[numscans] = xp2; ry2[numscans] = yp2;
            xb1[numscans] = (int16_t)bx1; xb2[numscans] = (int16_t)bx2;
            {
                int32_t t1 = wall_t_at(numscans, bx1), t2 = wall_t_at(numscans, bx2);
                int32_t d1 = yp1 + mulscale16(t1, yp2 - yp1), d2 = yp1 + mulscale16(t2, yp2 - yp1);
                st1[numscans] = t1; st2[numscans] = t2;
                yb1[numscans] = d1 < 256 ? 256 : d1;
                yb2[numscans] = d2 < 256 ? 256 : d2;
            }
            p2[numscans] = (int16_t)(numscans + 1);
            numscans++;
skipitaddwall:
            if ((wall[z].point2 < z) && (scanfirst < numscans))
                p2[numscans - 1] = (int16_t)scanfirst, scanfirst = numscans;
        }

        for (z = numscansbefore; z < numscans; z++)
            if ((wall[thewall[z]].point2 != thewall[p2[z]]) || (xb2[z] >= xb1[p2[z]])) {
                if (numbunches < MAXBUNCHES) bunchfirst[numbunches++] = p2[z];
                p2[z] = -1;
            }

        for (z = bunchfrst; z < numbunches; z++) {
            for (zz = bunchfirst[z]; p2[zz] >= 0; zz = p2[zz]);
            bunchlast[z] = (int16_t)zz;
        }
    } while (sectorbordercnt > 0);
}

static int32_t wallfront(int32_t l1, int32_t l2)
{
    walltype *wal;
    int32_t x11, y11, x21, y21, x12, y12, x22, y22, dx, dy, t1, t2;

    wal = &wall[thewall[l1]]; x11 = wal->x; y11 = wal->y;
    wal = &wall[wal->point2]; x21 = wal->x; y21 = wal->y;
    wal = &wall[thewall[l2]]; x12 = wal->x; y12 = wal->y;
    wal = &wall[wal->point2]; x22 = wal->x; y22 = wal->y;

    dx = x21 - x11; dy = y21 - y11;
    t1 = dmulscale2(x12 - x11, dy, -dx, y12 - y11);
    t2 = dmulscale2(x22 - x11, dy, -dx, y22 - y11);
    if (t1 == 0) { t1 = t2; if (t1 == 0) return -1; }
    if (t2 == 0) t2 = t1;
    if ((t1 ^ t2) >= 0) {
        t2 = dmulscale2(posx - x11, dy, -dx, posy - y11);
        return ((t2 ^ t1) >= 0);
    }

    dx = x22 - x12; dy = y22 - y12;
    t1 = dmulscale2(x11 - x12, dy, -dx, y11 - y12);
    t2 = dmulscale2(x21 - x12, dy, -dx, y21 - y12);
    if (t1 == 0) { t1 = t2; if (t1 == 0) return -1; }
    if (t2 == 0) t2 = t1;
    if ((t1 ^ t2) >= 0) {
        t2 = dmulscale2(posx - x12, dy, -dx, posy - y12);
        return ((t2 ^ t1) < 0);
    }
    return -2;
}

static int32_t spritewallfront(const spritetype *s, int32_t w)
{
    const walltype *wal = &wall[w];
    int32_t x1 = wal->x, y1 = wal->y;
    wal = &wall[wal->point2];
    return (dmulscale32(wal->x - x1, s->y - y1, -(s->x - x1), wal->y - y1) >= 0);
}

static int32_t bunchfront(int32_t b1, int32_t b2)
{
    int32_t x1b1, x2b1, x1b2, x2b2, b1f, b2f, i;

    b1f = bunchfirst[b1]; x1b1 = xb1[b1f]; x2b2 = xb2[bunchlast[b2]] + 1;
    if (x1b1 >= x2b2) return -1;
    b2f = bunchfirst[b2]; x1b2 = xb1[b2f]; x2b1 = xb2[bunchlast[b1]] + 1;
    if (x1b2 >= x2b1) return -1;

    if (x1b1 >= x1b2) {
        for (i = b2f; xb2[i] < x1b1; i = p2[i]);
        return wallfront(b1f, i);
    }
    for (i = b1f; xb2[i] < x1b2; i = p2[i]);
    return wallfront(i, b2f);
}

/* ------------------------------------------------- per-wall geometry */
static inline int64_t fast_div64(int64_t num, int32_t den)
{
    int64_t hi = num >> 31;
    int32_t ad = den < 0 ? -den : den;
    if (hi > -ad && hi < ad) return div64(num, den);
    return num / den;
}

static inline int32_t proj_y16(int32_t dz, int32_t d)
{
    int64_t num = mul64(dz, ysc);
    int64_t lim = (int64_t)d * 16640;
    if (num >= lim) return 0x3fff0000;
    if (num <= -lim) return -0x3fff0000;
    int32_t v = div64(num << 16, d) + (horizon << 16);
    if (v > 0x3fff0000) v = 0x3fff0000;
    if (v < -0x3fff0000) v = -0x3fff0000;
    return v;
}

static __attribute__((noinline)) int32_t proj_y16_ni(int32_t dz, int32_t d)
{
    int64_t num = mul64(dz, ysc);
    int64_t lim = (int64_t)d * 16640;
    if (num >= lim) return 0x3fff0000;
    if (num <= -lim) return -0x3fff0000;
    int32_t v = div64(num << 16, d) + (horizon << 16);
    if (v > 0x3fff0000) v = 0x3fff0000;
    if (v < -0x3fff0000) v = -0x3fff0000;
    return v;
}

/* Ceiling (dastat 0) / floor (1) line of `sectnum` along scan w into
 * mostbuf[]; returns Ken's "bad" bits (1/2: above top at L/R, 4/8: below
 * bottom at L/R). */
static int32_t wallmost(int16_t *mostbuf, int32_t w, int32_t sectnum, int dastat)
{
    const sectortype *sec = &sector[sectnum];
    int32_t z1, z2, stat = dastat ? sec->floorstat : sec->ceilingstat;
    int32_t ix1 = xb1[w], ix2 = xb2[w], x, bad = 0;

    if (stat & 2) {
        const walltype *wal = &wall[thewall[w]], *wal2 = &wall[wal->point2];
        int32_t dx = wal2->x - wal->x, dy = wal2->y - wal->y;
        int32_t wx1 = wal->x + mulscale16(st1[w], dx), wy1 = wal->y + mulscale16(st1[w], dy);
        int32_t wx2 = wal->x + mulscale16(st2[w], dx), wy2 = wal->y + mulscale16(st2[w], dy);
        if (dastat) { z1 = getflorzofslope(sectnum, wx1, wy1); z2 = getflorzofslope(sectnum, wx2, wy2); }
        else { z1 = getceilzofslope(sectnum, wx1, wy1); z2 = getceilzofslope(sectnum, wx2, wy2); }
    } else {
        z1 = z2 = dastat ? sec->floorz : sec->ceilingz;
    }
    int32_t y1 = proj_y16(z1 - posz, yb1[w]);
    int32_t y2 = proj_y16(z2 - posz, yb2[w]);
    if (y1 < 0) bad |= 1;
    if (y2 < 0) bad |= 2;
    if (y1 > (ydim << 16)) bad |= 4;
    if (y2 > (ydim << 16)) bad |= 8;

    int32_t yinc = (ix2 > ix1) ? div32(y2 - y1, ix2 - ix1) : 0;
    int32_t y = y1 + 32768;
    for (x = ix1; x <= ix2; x++, y += yinc) {
        int32_t v = y >> 16;
        if (v < 0) v = 0;
        if (v > ydim) v = ydim;
        mostbuf[x] = (int16_t)v;
    }
    return bad;
}

static void prepwall(int32_t z, const walltype *wal)
{
    int32_t x, walxrepeat = (wal->xrepeat << 3);
    int32_t d1 = ry1[z], dd = ry2[z] - ry1[z];
    int32_t a1 = rx1[z] >> 4, b1 = ry1[z] >> 4, a2 = rx2[z] >> 4, b2 = ry2[z] >> 4;
    int32_t xb = xb1[z], xe = xb2[z];
    int32_t xc2 = 2 * xb + 1 - xdim;
    int64_t num = (int64_t)xc2 * b1 - (int64_t)xdim * a1;
    int64_t den = (int64_t)xdim * (a2 - a1) - (int64_t)xc2 * (b2 - b1);
    int32_t dn = 2 * b1, ddn = -2 * (b2 - b1);
    int flipx = (wal->cstat & 8) != 0;
    int32_t umax = walxrepeat - 1;
    for (x = xb; x <= xe; x++, num += dn, den += ddn) {
        if (umost[x] >= dmost[x]) continue;
        int32_t t;
        if (x == xb) t = st1[z];
        else if (x == xe) t = st2[z];
        else if (!den) t = 0;
        else {
            int64_t n = num, d = den;
            while (d > 0x3fffffff || d < -0x3fffffff) { d >>= 1; n >>= 1; }
            t = div64(n << 16, (int32_t)d);
            if (t < 0) t = 0; else if (t > 65536) t = 65536;
        }
        int32_t d = d1 + mulscale16(t, dd);
        if (d < 256) d = 256;
        dcol[x] = d;
        int32_t u = (int32_t)(mul64(t, walxrepeat) >> 16);
        if (u > umax) u = umax;
        if (u < 0) u = 0;
        lwall[x] = flipx ? (umax - u) : u;
    }
}

/* ------------------------------------------------------ inner loops */
static void vline(pixel_t *dst, int32_t cnt, const uint8_t *col, uint32_t v, uint32_t dv,
                  uint32_t mask, const uint8_t *pal)
{
    int32_t p = pitch;
    if (cnt & 1) {
        uint32_t c = pal[col[(v >> 16) & mask]];
        *dst = (pixel_t)(c * 257u);
        dst += p;
        v += dv;
    }
    int32_t pairs = cnt >> 1;
    if (pairs > 0) {
        int32_t p2 = p + p;
        uint32_t dv2 = dv + dv;
        uint32_t v1 = v + dv;
        pixel_t *dst1 = dst + p;
        do {
            uint32_t c0 = pal[col[(v >> 16) & mask]];
            uint32_t c1 = pal[col[(v1 >> 16) & mask]];
            v += dv2;
            v1 += dv2;
            *dst = (pixel_t)(c0 * 257u);
            *dst1 = (pixel_t)(c1 * 257u);
            dst += p2;
            dst1 += p2;
        } while (--pairs);
    }
}

static __attribute__((noinline)) void mvline(pixel_t *dst, int32_t cnt, const uint8_t *col, uint32_t v, uint32_t dv,
                   uint32_t mask, const uint8_t *pal, int trans, int x, int y)
{
    int32_t p = pitch;
    if (!trans) {
        do {
            uint32_t t = col[(v >> 16) & mask];
            if (t != 255) {
                uint32_t c = pal[t];
                *dst = (pixel_t)((c << 8) | c);
            }
            dst += p;
            v += dv;
        } while (--cnt > 0);
    } else {
        if (!((x ^ y) & 1)) {
            dst += p;
            v += dv;
            if (--cnt <= 0) return;
        }
        p <<= 1;
        dv <<= 1;
        do {
            uint32_t t = col[(v >> 16) & mask];
            if (t != 255) {
                uint32_t c = pal[t];
                *dst = (pixel_t)((c << 8) | c);
            }
            dst += p;
            v += dv;
            cnt -= 2;
        } while (cnt > 0);
    }
}

/* Sky column angles depend only on the view width, not on the camera or
 * sector. Cache the exact Build getangle result instead of dividing again for
 * every sky column, for every visible parallax sector, on every frame. */

/* --------------------------------------------------------- wallscan */
#ifdef RDEBUG_PIC
int16_t RNAME(rdbg_pic)[RMAXYDIM][RMAXXDIM];
#define rdbg_pic RNAME(rdbg_pic)
static int16_t rdbg_cur;
#define RDBG_MARK(x, y1, y2) do { for (int _y = (y1); _y < (y2); _y++) rdbg_pic[_y][x] = rdbg_cur; } while (0)
#else
#define RDBG_MARK(x, y1, y2) ((void)0)
#endif
typedef struct {
    int32_t picnum, xpanning, ypanning, shade, pal, cstat, zrel, yrepeat, vis;
} wallpart_t;

static int32_t cur_wallnum = -1;
static void wallscan(int32_t x1, int32_t x2, const int16_t *uwal, const int16_t *dwal, const wallpart_t *wp,
                     int masked)
{
    int32_t x, picnum = picwrap(wp->picnum);
    if (PICANM(picnum) & 192) picnum = picwrap(picnum + animofs(picnum, 0));
    if (picnum == MIRROR) {
        if (!(rview.flags & RVF_MIRRORPASS)) {
            if (STATS.mirror_wall < 0 || x1 < STATS.mirror_x1) STATS.mirror_x1 = x1;
            if (STATS.mirror_wall < 0 || x2 > STATS.mirror_x2) STATS.mirror_x2 = x2;
            STATS.mirror_wall = cur_wallnum;
        }
        return;                 /* glass: leave the reflection drawn before */
    }
    const dk_tile_t *t = &tiledir[picnum];
    if (!(t->flags & DKT_PRESENT) || t->ow == 0) return;
    int32_t ow = t->ow, oh = t->oh, sh = t->shift;
    int32_t hmask = (1 << t->lh) - 1;
    const uint8_t *pix = (const uint8_t *)DKPTR(t->off);
    int32_t trans = masked && (wp->cstat & 128);

    /* texel rows per z unit = yrepeat/2048 -> 16.16: yrepeat*32 */
    uint32_t v0 = (uint32_t)(wp->zrel * wp->yrepeat * 32) +
                  ((uint32_t)(wp->ypanning << (t->lh + sh)) << 8);
    (void)oh;
    /* dv16 = depth_yp * yrepeat * 32 / ysc */
    int32_t kdv = div32(wp->yrepeat * 32 * 4096, ysc);     /* <<12 */
    int yflip = (wp->cstat & 256) != 0;
    int32_t ow_mask = ((ow & (ow - 1)) == 0) ? (ow - 1) : -1;
    const uint8_t *palbase = palookup[(uint32_t)wp->pal < DK_MAXPALS ? (uint32_t)wp->pal : 0u];
    int32_t nv = nightvis, wshade = wp->shade, wvis = wp->vis, wpan = wp->xpanning, th = t->h;

    for (x = x1; x <= x2; x++) {
        int32_t y1 = max(uwal[x], umost[x]);
        int32_t y2 = min(dwal[x], dmost[x]);
        if (y2 <= y1) continue;
        int32_t u = lwall[x] + wpan;
        if (ow_mask >= 0) u &= ow_mask;
        else if (u >= ow) { u -= ow; if (u >= ow) u -= div32(u, ow) * ow; }
        int32_t dx = dcol[x];
        int32_t dv = (int32_t)(mul64(dx, kdv) >> 12);
        uint32_t v = v0 + (uint32_t)((y1 - horizon) * dv + (dv >> 1));
        if (yflip) { v = (uint32_t)(-(int32_t)v); dv = -dv; }
        if (sh == 1) { v = (uint32_t)((int32_t)v >> 1); dv >>= 1; u >>= 1; }
        else if (sh) { v = (uint32_t)((int32_t)v >> sh); dv >>= sh; u >>= sh; }
        const uint8_t *col = pix + u * th;
        const uint8_t *pal = palbase;
        if (!nv) {
            int32_t s = wshade + (int32_t)(mul64(dx, wvis) >> 27);
            if (s < 0) s = 0;
            else if (s > DK_NUMSHADES - 1) s = DK_NUMSHADES - 1;
            pal += (s << 8);
        }
#ifdef RDEBUG_PIC
        rdbg_cur = (int16_t)(picnum | (masked ? 0x4000 : 0));
#endif
        RDBG_MARK(x, y1, y2);
        if (masked)
            mvline(frame + y1 * pitch + x, y2 - y1, col, v, (uint32_t)dv, hmask, pal, trans, x, y1);
        else {
            if (y1 < sky_done_y[x]) sky_done_y[x] = 0;
            vline(frame + y1 * pitch + x, y2 - y1, col, v, (uint32_t)dv, hmask, pal);
        }
        STATS.cols++;
    }
}

/* ------------------------------------------------- flat floors/ceils */
static struct {
    const uint8_t *pix;
    int32_t lw, lh, umask, vmask, shade, pal, vis, dz;
    int64_t dz_ysc2;
    int32_t baseu, basev, fu, ru, fv, rv;   /* texture mapping */
    int32_t ax, ay, bx, by;                 /* tex16 per world unit */
} fl;

static inline uint32_t shl_lh(uint32_t x, int32_t lh)
{
    if (lh == 6) return x << 6;
    if (lh == 5) return x << 5;
    if (lh == 4) return x << 4;
    if (lh == 3) return x << 3;
    if (lh == 7) return x << 7;
    if (lh == 8) return x << 8;
    return x << lh;
}

static void hline(int32_t xr, int32_t yp)
{
    int32_t xl = lastx[yp];
    if (xl > xr) return;
    int32_t dy2 = 2 * yp + 1 - 2 * horizon;
    if (dy2 == 0) return;
    /* depth (yp units) of this row = dz*ysc*2/dy2; overlap DIVU with setup */
#if IS_32X
    SH2_DVSR = dy2;
    SH2_DVDNTH = (int32_t)(fl.dz_ysc2 >> 32);
    SH2_DVDNTL = (int32_t)fl.dz_ysc2;
#endif
    int32_t xc2 = 2 * xl + 1 - xdim;                 /* 2*(x+0.5-halfx) */
    int32_t ufac = fl.fu + ((fl.ru * xc2) >> 1);
    int32_t vfac = fl.fv + ((fl.rv * xc2) >> 1);
    pixel_t *dst = frame + yp * pitch + xl;
    int32_t n = xr - xl + 1;
    int32_t lh = fl.lh;
    const uint8_t *pix = fl.pix;
    uint32_t umask = fl.umask, vmask = fl.vmask;
    const uint8_t *pal = palookup[(uint32_t)fl.pal < DK_MAXPALS ? (uint32_t)fl.pal : 0u];
#if IS_32X
    int32_t d = SH2_DVDNTL;
#else
    int32_t d = div64(fl.dz_ysc2, dy2);
#endif
    if (d <= 0) return;
    if (!nightvis) {
        int32_t s = fl.shade + (int32_t)(mul64(d, fl.vis) >> 27);
        if (s < 0) s = 0; else if (s > DK_NUMSHADES - 1) s = DK_NUMSHADES - 1;
        pal += (s << 8);
    }
    int32_t u = fl.baseu + (int32_t)(mul64(d, ufac) >> 22);
    int32_t v = fl.basev + (int32_t)(mul64(d, vfac) >> 22);
    int32_t du = (int32_t)(mul64(d, fl.ru) >> 22);
    int32_t dv = (int32_t)(mul64(d, fl.rv) >> 22);
    uint32_t uu = shl_lh((uint32_t)u, lh), duu = shl_lh((uint32_t)du, lh);
    uint32_t vv = (uint32_t)v, dvv = (uint32_t)dv;
    if (n >= 2 && ((uintptr_t)dst & 2)) {
        uint32_t c = pal[pix[((uu >> 16) & umask) + ((vv >> 16) & vmask)]];
        *dst++ = (pixel_t)((c << 8) | c);
        uu += duu; vv += dvv;
        n--;
    }
    while (n >= 2) {
        uint32_t c0 = pal[pix[((uu >> 16) & umask) + ((vv >> 16) & vmask)]];
        uu += duu; vv += dvv;
        uint32_t c1 = pal[pix[((uu >> 16) & umask) + ((vv >> 16) & vmask)]];
        uu += duu; vv += dvv;
        uint32_t p0 = (c0 << 8) | c0, p1 = (c1 << 8) | c1;
#if BIG_ENDIAN_DATA
        *(uint32_t *)dst = (p0 << 16) | p1;
#else
        *(uint32_t *)dst = (p1 << 16) | p0;
#endif
        dst += 2;
        n -= 2;
    }
    if (n > 0) {
        uint32_t c = pal[pix[((uu >> 16) & umask) + ((vv >> 16) & vmask)]];
        *dst = (pixel_t)((c << 8) | c);
    }
    STATS.spans++;
}

/* Build floor/ceiling texture orientation -> linear map from world */
static int setup_flat(const sectortype *sec, int dastat, int32_t picnum, int32_t stat,
                      int32_t xpan, int32_t ypan)
{
    picnum = picwrap(picnum);
    if (PICANM(picnum) & 192) picnum = picwrap(picnum + animofs(picnum, 0));
    const dk_tile_t *t = &tiledir[picnum];
    if (!(t->flags & DKT_PRESENT)) return 0;
    int32_t sh = t->shift;
    fl.pix = (const uint8_t *)DKPTR(t->off);
    fl.lw = t->lw; fl.lh = t->lh;
    fl.umask = ((1 << t->lw) - 1) << t->lh;
    fl.vmask = (1 << t->lh) - 1;
    /* world units per texel: 16 (8 with "double smooshiness") * 2^shift */
    int32_t sc = 4096 >> sh;                 /* 16.16 texels per world unit */
    if (stat & 8) sc <<= 1;
    /* texture axes in world space (ax,ay)=u axis, (bx,by)=v axis, scaled */
    int32_t ax, ay, bx, by, ox = 0, oy = 0;
    if (stat & 64) {                         /* relative alignment */
        const walltype *w0 = &wall[sec->wallptr], *w1 = &wall[w0->point2];
        int32_t dx = w1->x - w0->x, dy = w1->y - w0->y;
        int32_t len = ksqrt((uint32_t)(dx * dx + dy * dy));
        if (len == 0) len = 1;
        ax = div32(dx * 16384, len); ay = div32(dy * 16384, len);   /* unit along wall */
        bx = -ay; by = ax;
        ox = w0->x; oy = w0->y;
        /* u along the wall, v perpendicular (into the sector) */
        int32_t tx = ax, ty = ay;
        ax = mulscale14(tx, sc); ay = mulscale14(ty, sc);
        bx = mulscale14(-ty, sc); by = mulscale14(tx, sc);
        /* Build's relative mapping runs u along the wall, v to the right */
        bx = -bx; by = -by;
    } else {
        ax = sc; ay = 0; bx = 0; by = sc;
        bx = 0; by = -sc;                    /* Build: v runs along -y */
    }
    if (stat & 4) { int32_t tx = ax, ty = ay; ax = bx; ay = by; bx = tx; by = ty; }
    if (stat & 16) { ax = -ax; ay = -ay; }
    if (stat & 32) { bx = -bx; by = -by; }
    fl.ax = ax; fl.ay = ay; fl.bx = bx; fl.by = by;
    /* u(world) = ax*(wx-ox) + ay*(wy-oy);  world(x) = cam + d/256*(f + r*xc/halfx)/16384 */
    int32_t cxu = posx - ox, cyu = posy - oy;
    fl.baseu = ax * cxu + ay * cyu;
    fl.basev = bx * cxu + by * cyu;
    /* panning: 1/256 of the (original) tile size */
    fl.baseu += (int32_t)((uint32_t)(xpan * (1 << (t->lw + sh))) << 8) >> sh;
    fl.basev += (int32_t)((uint32_t)(ypan * (1 << (t->lh + sh))) << 8) >> sh;
    /* per depth (yp units = 256*xy): f = forward, r = right/halfx  (14-bit trig) */
    /* d*(ax*cos + ay*sin)/(256*16384) -> shift 22 total with the >>22 in hline */
    fl.fu = (ax * cosang + ay * sinang) >> 0;
    fl.fv = (bx * cosang + by * sinang) >> 0;
    fl.ru = div32(ay * cosang - ax * sinang, halfx);
    fl.rv = div32(by * cosang - bx * sinang, halfx);
    /* scale: ax*cos is (16.16 tex/unit)*(2^14): d (×256) * that >> 22 = tex16 */
    return 1;
}

static void flatscan(int32_t x1, int32_t x2, int32_t sectnum, int dastat)
{
    if (dastat == 0 && sky_done_key != 0xffffffffu) for (int32_t sx = x1; sx <= x2; sx++) sky_done_y[sx] = 0;
    const sectortype *sec = &sector[sectnum];
    int32_t x, y1, y2, twall, bwall;
    int32_t stat = dastat ? sec->floorstat : sec->ceilingstat;
    fl.dz = (dastat ? sec->floorz : sec->ceilingz) - posz;
    if (dastat ? (fl.dz <= 0) : (fl.dz >= 0)) return;
    if (dastat == 0) { while (x1 <= x2 && umost[x1] >= min(dmost[x1], uplc[x1])) x1++; }
    else { while (x1 <= x2 && max(umost[x1], dplc[x1]) >= dmost[x1]) x1++; }
    if (x1 > x2) return;
    fl.dz_ysc2 = mul64(fl.dz, ysc) * 2;
    if (!setup_flat(sec, dastat, dastat ? sec->floorpicnum : sec->ceilingpicnum, stat,
                    dastat ? sec->floorxpanning : sec->ceilingxpanning,
                    dastat ? sec->floorypanning : sec->ceilingypanning))
        return;
    /* fu/fv/ru/rv are tex16*2^14 per world unit; d is world*256 -> >>22 */
    fl.shade = dastat ? sec->floorshade : sec->ceilingshade;
    fl.pal = dastat ? sec->floorpal : sec->ceilingpal;
    fl.vis = sectvis(sec);

    y1 = dastat ? max(dplc[x1], umost[x1]) : umost[x1];
    y2 = y1;
    for (x = x1; x <= x2; x++) {
        if (dastat) { twall = max(dplc[x], umost[x]) - 1; bwall = dmost[x]; }
        else { twall = umost[x] - 1; bwall = min(uplc[x], dmost[x]); }
        if (twall < bwall - 1) {
            if (twall >= y2) {
                while (y1 < y2 - 1) hline(x - 1, ++y1);
                y1 = twall;
            } else {
                while (y1 < twall) hline(x - 1, ++y1);
                while (y1 > twall) lastx[y1--] = (int16_t)x;
            }
            while (y2 > bwall) hline(x - 1, --y2);
            while (y2 < bwall) lastx[y2++] = (int16_t)x;
        } else {
            while (y1 < y2 - 1) hline(x - 1, ++y1);
            if (x == x2) break;
            y1 = dastat ? max(dplc[x + 1], umost[x + 1]) : umost[x + 1];
            y2 = y1;
        }
    }
    while (y1 < y2 - 1) hline(x2, ++y1);
}

/* ------------------------------------------------------ sloped flats */
typedef struct {
    int64_t z0_16;
    int32_t rdx, rdy, gq, shade, vis, k16_4, yb;
    const uint8_t *palbase;
} slope_col_t;

static inline int32_t div_small(int32_t v, int32_t seg)
{
    if (seg <= 1) return v;
    if (seg == 2) return (v + ((uint32_t)v >> 31)) >> 1;
    if (seg == 4) return (v + (int32_t)(((uint32_t)(v >> 31)) & 3u)) >> 2;
    return div32(v, seg);
}

static __attribute__((noinline)) void slopescan_col(int32_t x, int32_t ya, const slope_col_t *sc)
{
    int32_t yb = sc->yb;
    int32_t k16_4 = sc->k16_4;
    int32_t rdx = sc->rdx, rdy = sc->rdy, gq = sc->gq;
    int64_t z0_16 = sc->z0_16;
    int32_t y = ya;
    pixel_t *dst = frame + ya * pitch + x;
    const uint8_t *pix = fl.pix;
    uint32_t umask = fl.umask, vmask = fl.vmask;
    int32_t lh = fl.lh, p = pitch, nv = nightvis;
    int32_t seg = yb - y; if (seg > 8) seg = 8;
    int32_t yy = y + seg;
    int32_t dw0;
#if IS_32X
    dw0 = SH2_DVDNTL;
#else
    {
        int32_t dy2a = 2 * y + 1 - 2 * horizon;
        int32_t den = mulscale8(dy2a, k16_4) - gq; if (den == 0) den = 1;
        dw0 = div64(z0_16, den);
    }
#endif
    if (dw0 < 1) dw0 = 1; else if (dw0 > 0x3fffff) dw0 = 0x3fffff;
    if (yb - y > 1) {
        int32_t dy2b = 2 * yy + 1 - 2 * horizon;
        int32_t denb = mulscale8(dy2b, k16_4) - gq; if (denb == 0) denb = 1;
#if IS_32X
        SH2_DVSR = denb;
        SH2_DVDNTH = (int32_t)(z0_16 >> 32);
        SH2_DVDNTL = (int32_t)z0_16;
#else
        (void)denb;
#endif
    }
    int32_t u0, v0, d0;
    {
        int32_t ddx = (int32_t)(mul64(dw0, rdx) >> 14);
        int32_t ddy = (int32_t)(mul64(dw0, rdy) >> 14);
        d0 = dw0 << 8;
        u0 = fl.baseu + fl.ax * ddx + fl.ay * ddy;
        v0 = fl.basev + fl.bx * ddx + fl.by * ddy;
    }
    if (yb - y == 1) {
        const uint8_t *palp = sc->palbase;
        if (!nv) {
            int32_t s = sc->shade + (int32_t)(mul64(d0, sc->vis) >> 27);
            if (s < 0) s = 0; else if (s > DK_NUMSHADES - 1) s = DK_NUMSHADES - 1;
            palp += (s << 8);
        }
        uint32_t u = shl_lh((uint32_t)u0, lh), v = (uint32_t)v0;
        uint32_t c = palp[pix[((u >> 16) & umask) + ((v >> 16) & vmask)]];
        *dst = (pixel_t)(c * 257u);
        return;
    }
    int32_t u1, v1, d1;
    {
        int32_t dw;
#if IS_32X
        dw = SH2_DVDNTL;
#else
        int32_t dy2b = 2 * yy + 1 - 2 * horizon;
        int32_t den = mulscale8(dy2b, k16_4) - gq; if (den == 0) den = 1;
        dw = div64(z0_16, den);
#endif
        if (dw < 1) dw = 1; else if (dw > 0x3fffff) dw = 0x3fffff;
        int32_t ddx = (int32_t)(mul64(dw, rdx) >> 14);
        int32_t ddy = (int32_t)(mul64(dw, rdy) >> 14);
        d1 = dw << 8;
        u1 = fl.baseu + fl.ax * ddx + fl.ay * ddy;
        v1 = fl.basev + fl.bx * ddx + fl.by * ddy;
    }
    for (;;) {
        int32_t seg2 = yb - yy; if (seg2 > 8) seg2 = 8;
        int32_t yy2 = yy + seg2;
        if (seg2 > 0) {
            int32_t dy2c = 2 * yy2 + 1 - 2 * horizon;
            int32_t k16c = mulscale8(dy2c, k16_4);
            int32_t denc = k16c - gq; if (denc == 0) denc = 1;
#if IS_32X
            SH2_DVSR = denc;
            SH2_DVDNTH = (int32_t)(z0_16 >> 32);
            SH2_DVDNTL = (int32_t)z0_16;
#else
            (void)denc;
#endif
        }
        int32_t du = (seg == 8) ? ((u1 - u0) >> 3) : div_small(u1 - u0, seg);
        int32_t dv = (seg == 8) ? ((v1 - v0) >> 3) : div_small(v1 - v0, seg);
        const uint8_t *palp = sc->palbase;
        if (!nv) {
            int32_t s = sc->shade + (int32_t)(mul64(d0, sc->vis) >> 27);
            if (s < 0) s = 0; else if (s > DK_NUMSHADES - 1) s = DK_NUMSHADES - 1;
            palp += (s << 8);
        }
        uint32_t u = shl_lh((uint32_t)u0, lh), duu = shl_lh((uint32_t)du, lh);
        uint32_t v = (uint32_t)v0;
        for (int n = 0; n < seg; n++) {
            uint32_t c = palp[pix[((u >> 16) & umask) + ((v >> 16) & vmask)]];
            *dst = (pixel_t)(c * 257u);
            dst += p;
            u += duu; v += (uint32_t)dv;
        }
        u0 = u1; v0 = v1; d0 = d1;
        y = yy;
        if (seg2 <= 0) break;
        seg = seg2; yy = yy2;
        int32_t dw;
#if IS_32X
        dw = SH2_DVDNTL;
#else
        {
            int32_t dy2c = 2 * yy + 1 - 2 * horizon;
            int32_t k16c = mulscale8(dy2c, k16_4);
            int32_t denc = k16c - gq; if (denc == 0) denc = 1;
            dw = div64(z0_16, denc);
        }
#endif
        if (dw < 1) dw = 1; else if (dw > 0x3fffff) dw = 0x3fffff;
        int32_t ddx = (int32_t)(mul64(dw, rdx) >> 14);
        int32_t ddy = (int32_t)(mul64(dw, rdy) >> 14);
        d1 = dw << 8;
        u1 = fl.baseu + fl.ax * ddx + fl.ay * ddy;
        v1 = fl.basev + fl.bx * ddx + fl.by * ddy;
    }
}

static void slopescan(int32_t x1, int32_t x2, int32_t sectnum, int dastat, int16_t *ray_dx, int16_t *ray_dy, uint8_t *ray_ok)
{
    if (dastat == 0 && sky_done_key != 0xffffffffu) for (int32_t sx = x1; sx <= x2; sx++) sky_done_y[sx] = 0;
    const sectortype *sec = &sector[sectnum];
    int32_t stat = dastat ? sec->floorstat : sec->ceilingstat;
    int32_t heinum = dastat ? sec->floorheinum : sec->ceilingheinum;
    int32_t zcam = dastat ? getflorzofslope(sectnum, posx, posy) : getceilzofslope(sectnum, posx, posy);
    int32_t z0 = zcam - posz;
    if (dastat ? (z0 <= 0) : (z0 >= 0)) return;     /* back-face */
    /* Skip setup_flat/ksqrt/div64 when all columns in [x1..x2] are empty. */
    int32_t vx1 = x1;
    if (dastat == 0) {
        while (vx1 <= x2 && umost[vx1] >= min(dmost[vx1], uplc[vx1])) vx1++;
    } else {
        while (vx1 <= x2 && max(umost[vx1], dplc[vx1]) >= dmost[vx1]) vx1++;
    }
    if (vx1 > x2) return;
    if (!setup_flat(sec, dastat, dastat ? sec->floorpicnum : sec->ceilingpicnum, stat,
                    dastat ? sec->floorxpanning : sec->ceilingxpanning,
                    dastat ? sec->floorypanning : sec->ceilingypanning))
        return;

    const walltype *w0 = &wall[sec->wallptr], *w1 = &wall[w0->point2];
    int32_t wdx = w1->x - w0->x, wdy = w1->y - w0->y;
    int32_t len = ksqrt((uint32_t)(wdx * wdx + wdy * wdy));
    if (len == 0) return;
    int32_t gx = div64(mul64(-wdy, heinum) << 8, len);
    int32_t gy = div64(mul64(wdx, heinum) << 8, len);

    if (!*ray_ok) {
        int32_t xdim2 = 2 * halfx;
        for (int32_t rx = cx0; rx <= cx1; rx++) {
            int32_t xc2 = 2 * rx + 1 - xdim;
            ray_dx[rx] = (int16_t)(cosang - div32(sinang * xc2, xdim2));
            ray_dy[rx] = (int16_t)(sinang + div32(cosang * xc2, xdim2));
        }
        *ray_ok = 1;
    }

    slope_col_t sc;
    sc.z0_16 = (int64_t)z0 << 16;
    sc.shade = dastat ? sec->floorshade : sec->ceilingshade;
    int32_t pal = dastat ? sec->floorpal : sec->ceilingpal;
    sc.palbase = palookup[(uint32_t)pal < DK_MAXPALS ? (uint32_t)pal : 0u];
    sc.vis = sectvis(sec);
    sc.k16_4 = k16_6 << 2;

    for (int32_t x = vx1; x <= x2; x++) {
        int32_t ya, yb;
        if (dastat == 0) { ya = umost[x]; yb = min(dmost[x], uplc[x]); }
        else { ya = max(umost[x], dplc[x]); yb = dmost[x]; }
        if (yb <= ya) continue;
        RDBG(0x6000 + x);
        RDBG2((uint16_t)((ya << 8) | (yb & 0xff)));
        sc.rdx = ray_dx[x];
        sc.rdy = ray_dy[x];
        int64_t g = mul64(gx, sc.rdx) + mul64(gy, sc.rdy);
        sc.gq = (int32_t)(g >> 14);
        sc.yb = yb;
#if IS_32X
        {
            int32_t dena = mulscale8(2 * ya + 1 - 2 * horizon, sc.k16_4) - sc.gq;
            if (dena == 0) dena = 1;
            SH2_DVSR = dena;
            SH2_DVDNTH = (int32_t)(sc.z0_16 >> 32);
            SH2_DVDNTL = (int32_t)sc.z0_16;
        }
#endif
        slopescan_col(x, ya, &sc);
        STATS.cols++;
    }
}

/* -------------------------------------------------------- parallax */
/* Build parascan: the sky is a wall scanned at yrepeat 8 with a constant
 * scale, texture centre on a (parallaxyscale-damped) horizon, 8 slots of
 * 256 angle units each showing tile pic + pskyoff[slot]. */
static void parascan(int32_t x1, int32_t x2, int32_t sectnum, int dastat)
{
    const sectortype *sec = &sector[sectnum];
    int32_t pic = picwrap(dastat ? sec->floorpicnum : sec->ceilingpicnum);
    int32_t shade = dastat ? sec->floorshade : sec->ceilingshade;
    int32_t pal = dastat ? sec->floorpal : sec->ceilingpal;
    int32_t ypan = dastat ? sec->floorypanning : sec->ceilingypanning;
    const dk_tile_t *t0 = &tiledir[pic];
    if (!(t0->flags & DKT_PRESENT) || t0->ow <= 0) return;
    const uint8_t *palp = palptr(pal, nightvis ? 0 : max(0, min(31, shade)));
    int32_t yscale = rview.parallaxyscale ? rview.parallaxyscale : 65536;
    int32_t hz = horizon;
    if (yscale != 65536) hz = mulscale16(horizon - (ydim >> 1), yscale) + (ydim >> 1);
    /* 1 texel/row at Build's 320x200 (vfocal 128): here 128/vfocal */
    int32_t dv = div32(128 * 16 * 65536, ysc);
    if (skyangle_width != xdim) {
        for (int x = 0; x < xdim; x++)
            skyangle[x] = (int16_t)getangle(2 * halfx, 2 * x + 1 - xdim);
        skyangle_width = xdim;
    }
    static uint16_t sky_pal16[256];
    static const uint8_t *sky_pal_src;
    if (sky_pal_src != palp) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = palp[i];
            sky_pal16[i] = (uint16_t)((c << 8) | c);
        }
        sky_pal_src = palp;
    }
    uint32_t skey = 0xffffffffu;
    if (dastat == 0) {
        skey = ((uint32_t)(uintptr_t)palp ^ ((uint32_t)pic << 16) ^ ((uint32_t)(ypan & 255) << 8));
        if (skey == 0xffffffffu) skey = 0xfffffffeu;
        if (sky_done_key != skey) {
            for (int32_t sx = cx0; sx <= cx1; sx++) sky_done_y[sx] = 0;
            sky_done_key = skey;
        }
    }
    int32_t x, p = pitch;
    for (x = x1; x <= x2; x++) {
        int32_t ya, yb;
        if (dastat == 0) {
            ya = umost[x]; yb = min(uplc[x], dmost[x]);
            if (ya < sky_done_y[x]) ya = sky_done_y[x];
            if (yb <= ya) continue;
            sky_done_y[x] = (int16_t)yb;
        } else {
            ya = max(umost[x], dplc[x]); yb = dmost[x];
            if (yb <= ya) continue;
        }
        int32_t a = (rview.ang + skyangle[x]) & 2047;
        int32_t tp = picwrap(pic + rview.pskyoff[a >> 8]);
        const dk_tile_t *t = &tiledir[tp];
        if (!(t->flags & DKT_PRESENT)) t = t0;
        int32_t sh = t->shift;
        int32_t u = ((a & 255) * t->ow) >> 8;
        int32_t us = (sh == 1) ? (u >> 1) : (sh ? (u >> sh) : u);
        const uint8_t *col = (const uint8_t *)DKPTR(t->off) + us * t->h;
        int32_t v = ((t->oh >> 1) << 16) + ((ypan * t->oh) << 8) + (ya - hz) * dv + (dv >> 1);
        int32_t vmax = (t->oh << 16) - 65536;
        pixel_t *dst = frame + ya * p + x;
        int32_t rem = yb - ya;
        if (v < 0) {
            int32_t n = div32(-v + dv - 1, dv);
            if (n > rem) n = rem;
            uint16_t c0 = sky_pal16[col[0]];
            rem -= n;
            v += n * dv;
            do { *dst = c0; dst += p; } while (--n > 0);
        }
        if (rem > 0 && v <= vmax) {
            int32_t n;
            if (v + (rem - 1) * dv <= vmax) n = rem;
            else { n = div32(vmax - v, dv) + 1; if (n > rem) n = rem; }
            rem -= n;
            uint32_t uv = (uint32_t)v, udv = (uint32_t)dv;
            if (sh == 1) {
                while (n >= 4) {
                    dst[0] = sky_pal16[col[uv >> 17]]; dst += p; uv += udv;
                    dst[0] = sky_pal16[col[uv >> 17]]; dst += p; uv += udv;
                    dst[0] = sky_pal16[col[uv >> 17]]; dst += p; uv += udv;
                    dst[0] = sky_pal16[col[uv >> 17]]; dst += p; uv += udv;
                    n -= 4;
                }
                while (n-- > 0) { *dst = sky_pal16[col[uv >> 17]]; dst += p; uv += udv; }
            } else if (sh == 0) {
                do { *dst = sky_pal16[col[uv >> 16]]; dst += p; uv += udv; } while (--n > 0);
            } else {
                int32_t s16 = 16 + sh;
                do { *dst = sky_pal16[col[uv >> s16]]; dst += p; uv += udv; } while (--n > 0);
            }
            v = (int32_t)uv;
        }
        if (rem > 0) {
            int32_t iend = (sh == 1) ? ((uint32_t)vmax >> 17) : (sh ? ((uint32_t)vmax >> (16 + sh)) : ((uint32_t)vmax >> 16));
            uint16_t c1 = sky_pal16[col[iend]];
            do { *dst = c1; dst += p; } while (--rem > 0);
        }
    }
}

/* ---------------------------------------------------------- drawalls */
static void drawalls(int32_t bunch, int16_t *ray_dx, int16_t *ray_dy, uint8_t *ray_ok)
{
    sectortype *sec, *nextsec;
    walltype *wal;
    int32_t i, x, x1, x2, cz[5], fz[5];
    int32_t z, wallnum, sectnum, nextsectnum;
    int32_t startsmostwallcnt, startsmostcnt, gotswall;
    int32_t andwstat1, andwstat2;
    wallpart_t wp;

    int32_t bx1 = xb1[bunchfirst[bunch]], bx2 = xb2[bunchlast[bunch]];
    int32_t vx1 = bx2 + 1, vx2 = bx1 - 1;
    for (int32_t sx = bx1; sx <= bx2; sx++) {
        if (umost[sx] < dmost[sx]) {
            if (vx1 > bx2) vx1 = sx;
            vx2 = sx;
        }
    }
    if (vx1 > vx2) {
        for (z = bunchfirst[bunch]; z >= 0; z = p2[z]) {
            smostwall[smostwallcnt] = (int16_t)z;
            smostwalltype[smostwallcnt] = 0;
            smostwallcnt++;
        }
        return;
    }

    z = bunchfirst[bunch];
    sectnum = thesector[z]; sec = &sector[sectnum];

    andwstat1 = 0xff; andwstat2 = 0xff;
    for (; z >= 0; z = p2[z]) {
        RDBG(0x5000 + z);
        andwstat1 &= wallmost(uplc, z, sectnum, 0);
        andwstat2 &= wallmost(dplc, z, sectnum, 1);
    }
    RDBG(0xC00);
    if ((andwstat1 & 3) != 3) {
        if ((sec->ceilingstat & 3) == 2) { RDBG(0xC01); slopescan(vx1, vx2, sectnum, 0, ray_dx, ray_dy, ray_ok); }
        else if ((sec->ceilingstat & 1) == 0) { RDBG(0xC02); flatscan(vx1, vx2, sectnum, 0); }
        else { RDBG(0xC03); parascan(vx1, vx2, sectnum, 0); }
        if ((sec->ceilingstat & 1) == 0 && sky_done_key != 0xffffffffu)
            for (int32_t sx = bx1; sx <= bx2; sx++) sky_done_y[sx] = 0;
    }
    if ((andwstat2 & 12) != 12) {
        if ((sec->floorstat & 3) == 2) { RDBG(0xC04); slopescan(vx1, vx2, sectnum, 1, ray_dx, ray_dy, ray_ok); }
        else if ((sec->floorstat & 1) == 0) { RDBG(0xC05); flatscan(vx1, vx2, sectnum, 1); }
        else { RDBG(0xC06); parascan(vx1, vx2, sectnum, 1); }
    }

    int32_t vis = sectvis(sec);
    RDBG(0xD00);
    for (z = bunchfirst[bunch]; z >= 0; z = p2[z]) {
        x1 = xb1[z]; x2 = xb2[z];
        if (umost[x2] >= dmost[x2]) {
            for (x = x1; x < x2; x++)
                if (umost[x] < dmost[x]) break;
            if (x >= x2) {
                smostwall[smostwallcnt] = (int16_t)z;
                smostwalltype[smostwallcnt] = 0;
                smostwallcnt++;
                continue;
            }
        }

        wallnum = thewall[z]; wal = &wall[wallnum];
        cur_wallnum = wallnum;
        nextsectnum = wal->nextsector;
        nextsec = (nextsectnum >= 0) ? &sector[nextsectnum] : 0;
        gotswall = 0;
        startsmostwallcnt = smostwallcnt;
        startsmostcnt = smostcnt;
        STATS.walls++;

        if (nextsectnum >= 0) {
            if ((sec->ceilingstat | sec->floorstat) & 2) {
                getzsofslope((int16_t)sectnum, wal->x, wal->y, &cz[0], &fz[0]);
                getzsofslope((int16_t)sectnum, wall[wal->point2].x, wall[wal->point2].y, &cz[1], &fz[1]);
            } else {
                cz[0] = cz[1] = sec->ceilingz;
                fz[0] = fz[1] = sec->floorz;
            }
            if ((nextsec->ceilingstat | nextsec->floorstat) & 2) {
                getzsofslope((int16_t)nextsectnum, wal->x, wal->y, &cz[2], &fz[2]);
                getzsofslope((int16_t)nextsectnum, wall[wal->point2].x, wall[wal->point2].y, &cz[3], &fz[3]);
                getzsofslope((int16_t)nextsectnum, posx, posy, &cz[4], &fz[4]);
            } else {
                cz[2] = cz[3] = cz[4] = nextsec->ceilingz;
                fz[2] = fz[3] = fz[4] = nextsec->floorz;
            }

            if ((wal->cstat & 48) == 16 && maskwallcnt < MAXMASKWALLS) maskwall[maskwallcnt++] = (int16_t)z;

            if (((sec->ceilingstat & 1) == 0) || ((nextsec->ceilingstat & 1) == 0)) {
                if ((cz[2] <= cz[0]) && (cz[3] <= cz[1])) {
                    for (x = x1; x <= x2; x++)
                        if (uplc[x] > umost[x])
                            if (umost[x] <= dmost[x]) {
                                umost[x] = uplc[x];
                                if (umost[x] > dmost[x]) numhits--;
                            }
                } else {
                    wallmost(dwall, z, nextsectnum, 0);
                    if ((cz[2] > fz[0]) || (cz[3] > fz[1]))
                        for (i = x1; i <= x2; i++) if (dwall[i] > dplc[i]) dwall[i] = dplc[i];

                    wp.picnum = wal->picnum; wp.xpanning = wal->xpanning; wp.ypanning = wal->ypanning;
                    wp.shade = wal->shade; wp.pal = wal->pal; wp.cstat = wal->cstat; wp.vis = vis;
                    wp.yrepeat = wal->yrepeat;
                    wp.zrel = (wal->cstat & 4) ? (posz - sec->ceilingz) : (posz - nextsec->ceilingz);
                    if (gotswall == 0) { gotswall = 1; prepwall(z, wal); }
                    wallscan(x1, x2, uplc, dwall, &wp, 0);

                    if ((cz[2] >= cz[0]) && (cz[3] >= cz[1])) {
                        for (x = x1; x <= x2; x++)
                            if (dwall[x] > umost[x])
                                if (umost[x] <= dmost[x]) {
                                    umost[x] = dwall[x];
                                    if (umost[x] > dmost[x]) numhits--;
                                }
                    } else {
                        for (x = x1; x <= x2; x++)
                            if (umost[x] <= dmost[x]) {
                                i = max(uplc[x], dwall[x]);
                                if (i > umost[x]) {
                                    umost[x] = (int16_t)i;
                                    if (umost[x] > dmost[x]) numhits--;
                                }
                            }
                    }
                }
                if ((cz[2] < cz[0]) || (cz[3] < cz[1]) || (posz < cz[4])) {
                    i = x2 - x1 + 1;
                    if (smostcnt + i < MAXYSAVES) {
                        smoststart[smostwallcnt] = (int16_t)smostcnt;
                        smostwall[smostwallcnt] = (int16_t)z;
                        smostwalltype[smostwallcnt] = 1;
                        smostwallcnt++;
                        for (int q = 0; q < i; q++) smost[smostcnt + q] = (uint8_t)umost[x1 + q];
                        smostcnt += i;
                    }
                }
            }
            if (((sec->floorstat & 1) == 0) || ((nextsec->floorstat & 1) == 0)) {
                if ((fz[2] >= fz[0]) && (fz[3] >= fz[1])) {
                    for (x = x1; x <= x2; x++)
                        if (dplc[x] < dmost[x])
                            if (umost[x] <= dmost[x]) {
                                dmost[x] = dplc[x];
                                if (umost[x] > dmost[x]) numhits--;
                            }
                } else {
                    wallmost(uwall, z, nextsectnum, 1);
                    if ((fz[2] < cz[0]) || (fz[3] < cz[1]))
                        for (i = x1; i <= x2; i++) if (uwall[i] < uplc[i]) uwall[i] = uplc[i];

                    const walltype *tw = wal;
                    if ((wal->cstat & 2) && wallx[wallnum].nextwall >= 0) tw = &wall[wallx[wallnum].nextwall];
                    wp.picnum = tw->picnum; wp.xpanning = tw->xpanning; wp.ypanning = tw->ypanning;
                    wp.shade = tw->shade; wp.pal = tw->pal; wp.cstat = tw->cstat; wp.vis = vis;
                    wp.yrepeat = wal->yrepeat;
                    wp.zrel = (tw->cstat & 4) ? (posz - sec->ceilingz) : (posz - nextsec->floorz);
                    if (gotswall == 0) { gotswall = 1; prepwall(z, wal); }
                    wallscan(x1, x2, uwall, dplc, &wp, 0);

                    if ((fz[2] <= fz[0]) && (fz[3] <= fz[1])) {
                        for (x = x1; x <= x2; x++)
                            if (uwall[x] < dmost[x])
                                if (umost[x] <= dmost[x]) {
                                    dmost[x] = uwall[x];
                                    if (umost[x] > dmost[x]) numhits--;
                                }
                    } else {
                        for (x = x1; x <= x2; x++)
                            if (umost[x] <= dmost[x]) {
                                i = min(dplc[x], uwall[x]);
                                if (i < dmost[x]) {
                                    dmost[x] = (int16_t)i;
                                    if (umost[x] > dmost[x]) numhits--;
                                }
                            }
                    }
                }
                if ((fz[2] > fz[0]) || (fz[3] > fz[1]) || (posz > fz[4])) {
                    i = x2 - x1 + 1;
                    if (smostcnt + i < MAXYSAVES) {
                        smoststart[smostwallcnt] = (int16_t)smostcnt;
                        smostwall[smostwallcnt] = (int16_t)z;
                        smostwalltype[smostwallcnt] = 2;
                        smostwallcnt++;
                        for (int q = 0; q < i; q++) smost[smostcnt + q] = (uint8_t)dmost[x1 + q];
                        smostcnt += i;
                    }
                }
            }
            if (numhits < 0) return;
            if ((!(wal->cstat & 32)) && ((gotsector[nextsectnum >> 3] & pow2char[nextsectnum & 7]) == 0)) {
                if (umost[x2] < dmost[x2])
                    scansector((int16_t)nextsectnum);
                else {
                    for (x = x1; x < x2; x++)
                        if (umost[x] < dmost[x]) { scansector((int16_t)nextsectnum); break; }
                    if (x == x2) {
                        smostwallcnt = startsmostwallcnt;
                        smostcnt = startsmostcnt;
                        smostwall[smostwallcnt] = (int16_t)z;
                        smostwalltype[smostwallcnt] = 0;
                        smostwallcnt++;
                    }
                }
            }
        }
        if ((nextsectnum < 0) || (wal->cstat & 32)) {
            wp.picnum = (nextsectnum < 0) ? wal->picnum : wal->overpicnum;
            wp.xpanning = wal->xpanning; wp.ypanning = wal->ypanning;
            wp.shade = wal->shade; wp.pal = wal->pal; wp.cstat = wal->cstat; wp.vis = vis;
            wp.yrepeat = wal->yrepeat;
            if (nextsectnum >= 0)
                wp.zrel = (wal->cstat & 4) ? (posz - sec->ceilingz) : (posz - nextsec->ceilingz);
            else
                wp.zrel = (wal->cstat & 4) ? (posz - sec->floorz) : (posz - sec->ceilingz);
            if (gotswall == 0) { gotswall = 1; prepwall(z, wal); }
            wallscan(x1, x2, uplc, dplc, &wp, 0);

            for (x = x1; x <= x2; x++)
                if (umost[x] <= dmost[x]) { umost[x] = 1; dmost[x] = 0; numhits--; }
            smostwall[smostwallcnt] = (int16_t)z;
            smostwalltype[smostwallcnt] = 0;
            smostwallcnt++;
        }
    }
}

/* ----------------------------------------------------------- sprites */
static int16_t suwall[RMAXXDIM], sdwall[RMAXXDIM];

/* Clip window for a sprite/maskwall covering [lx,rx] at depth yp against
 * all walls drawn in front of it (Ken's smost logic). Returns 0 if hidden. */
static int sprite_clip(const spritetype *tspr, int32_t lx, int32_t rx, int32_t yp,
                       int32_t startum, int32_t startdm, int ismaskwall, int32_t mwz)
{
    int32_t i, j, k, x, dalx2, darx2, daclip = 0;
    for (x = lx; x <= rx; x++) {
        suwall[x] = (int16_t)max(0, startum);
        sdwall[x] = (int16_t)min(ydim, startdm);
    }
    for (i = smostwallcnt - 1; i >= 0; i--) {
        if (smostwalltype[i] & daclip) continue;
        j = smostwall[i];
        if ((xb1[j] > rx) || (xb2[j] < lx)) continue;
        if (ismaskwall) {
            if (j == mwz) continue;
            /* walls behind the mask wall don't clip it */
            if (wallfront(mwz, j) == 1 || ((yp <= yb1[j]) && (yp <= yb2[j]))) {
                int32_t f = wallfront(j, mwz);
                if (f != 1) continue;
            }
        } else {
            if ((yp <= yb1[j]) && (yp <= yb2[j])) continue;
            if (spritewallfront(tspr, (int32_t)thewall[j]) && ((yp <= yb1[j]) || (yp <= yb2[j]))) continue;
        }
        dalx2 = max(xb1[j], lx); darx2 = min(xb2[j], rx);
        switch (smostwalltype[i]) {
        case 0:
            if (dalx2 <= darx2) {
                if ((dalx2 == lx) && (darx2 == rx)) return 0;
                for (x = dalx2; x <= darx2; x++) sdwall[x] = 0;
            }
            break;
        case 1:
            k = smoststart[i] - xb1[j];
            for (x = dalx2; x <= darx2; x++)
                if (smost[k + x] > suwall[x]) suwall[x] = smost[k + x];
            if ((dalx2 == lx) && (darx2 == rx)) daclip |= 1;
            break;
        case 2:
            k = smoststart[i] - xb1[j];
            for (x = dalx2; x <= darx2; x++)
                if (smost[k + x] < sdwall[x]) sdwall[x] = smost[k + x];
            if ((dalx2 == lx) && (darx2 == rx)) daclip |= 2;
            break;
        }
    }
    for (x = lx; x <= rx; x++)
        if (suwall[x] < sdwall[x]) return 1;
    return 0;
}

static void drawsprite_face(int32_t snum)
{
    spritetype *tspr = &sprite[tsprite[snum]];
    int32_t yp = spritesy[snum];
    int16_t gpic = tspr->picnum; uint16_t gcs = tspr->cstat; int8_t gsh = tspr->shade;
    if (!game_tsprite(tsprite[snum], &gpic, &gcs, &gsh)) return;
    int32_t tilenum = gpic, cstat = gcs;
    if (yp <= (4 << 8)) return;
    if (PICANM(tilenum) & 192) tilenum = picwrap(tilenum + animofs((int16_t)tilenum, 0));
    tilenum = picwrap(tilenum);
    const dk_tile_t *t = &tiledir[tilenum];
    if (!(t->flags & DKT_PRESENT) || t->ow <= 0 || t->oh <= 0) return;
    const sectortype *sec = &sector[tspr->sectnum];

    int32_t xspan = t->ow, yspan = t->oh;
    int32_t xoff = t->xoff + tspr->xoffset, yoff = t->yoff + tspr->yoffset;
    /* world size: width = xspan*xrepeat/4 xy units, height = yspan*yrepeat*4 z units */
    int32_t xc = spritesx[snum];                         /* screen centre, 24.8 */
    /* pixels per xy unit at depth: halfx*256/yp ; sprite width in xy units: xspan*xrepeat/4 */
    int32_t wpx = div64(mul64((int64_t)xspan * tspr->xrepeat, halfx) << 14, yp);  /* 24.8 */
    /* height in rows: (yspan*yrepeat*4) z * ysc / yp */
    int32_t hpx = div64(mul64((int64_t)yspan * tspr->yrepeat * 4, ysc) << 8, yp);  /* 24.8 */
    if (wpx <= 0 || hpx <= 0) return;
    int32_t x1 = xc - (wpx >> 1);
    int32_t xo = div64(mul64((int64_t)xoff * tspr->xrepeat, halfx) << 14, yp);
    if (cstat & 4) x1 += xo; else x1 -= xo;

    int32_t zb = tspr->z - ((yoff * tspr->yrepeat) << 2);
    if (cstat & 128) zb += ((yspan * tspr->yrepeat) << 1);
    int32_t y2f = proj_y16_ni(zb - posz, yp) >> 8;          /* bottom, 24.8 */
    int32_t y1f = y2f - hpx;
    int32_t x2 = x1 + wpx;

    int32_t lx = (x1 + 255) >> 8, rx = (x2 - 1) >> 8;
    if (lx < cx0) lx = cx0;
    if (rx > cx1) rx = cx1;
    if (lx > rx) return;
    int32_t ty1 = (y1f + 255) >> 8, ty2 = (y2f + 255) >> 8;
    if (ty1 >= ydim || ty2 <= 0) return;

    int32_t startum = 0, startdm = ydim;
    if ((sec->ceilingstat & 3) == 0) startum = (proj_y16_ni(sec->ceilingz - posz, yp) >> 16) - 1;
    if ((sec->floorstat & 3) == 0) startdm = (proj_y16_ni(sec->floorz - posz, yp) >> 16) + 1;
    if (ty1 > startum) startum = ty1;
    if (ty2 < startdm) startdm = ty2;
    if (startum >= startdm) return;
    if (!sprite_clip(tspr, lx, rx, yp, startum, startdm, 0, 0)) return;

    int32_t sh = t->shift;
    const uint8_t *pix = (const uint8_t *)DKPTR(t->off);
    const uint8_t *spans = (t->flags & DKT_SPANS) ? pix + t->w * t->h : 0;
    /* texel steps (original units, 16.16) */
    int32_t dux = div64((int64_t)xspan << 24, wpx);      /* per screen column */
    int32_t dvy = div64((int64_t)yspan << 24, hpx);      /* per screen row    */
    int32_t u0 = (int32_t)(mul64(((lx << 8) + 128) - x1, dux) >> 8);
    const uint8_t *pal = palptr(tspr->pal, shadeidx(tspr->shade, yp, sectvis(sec)));
    int trans = (cstat & 2) != 0;
    if (cstat & 8) { /* y-flip */ }
    int32_t x;
    for (x = lx; x <= rx; x++, u0 += dux) {
        int32_t ya = suwall[x], yb = sdwall[x];
        if (ya < 0) ya = 0;
        if (yb > ydim) yb = ydim;
        if (yb <= ya) continue;
        int32_t u = u0 >> 16;
        if (u < 0 || u >= xspan) continue;
        if (cstat & 4) u = xspan - 1 - u;
        int32_t us = (sh == 1) ? (u >> 1) : (sh ? (u >> sh) : u);
        if (us >= t->w) us = t->w - 1;
        const uint8_t *col = pix + us * t->h;
        /* v at row ya */
        int32_t v = (int32_t)(mul64(((ya << 8) + 128) - y1f, dvy) >> 8);
        int32_t dv = dvy;
        if (cstat & 8) { v = (yspan << 16) - 1 - v; dv = -dv; }
        if (sh == 1) { v >>= 1; dv >>= 1; }
        else if (sh) { v >>= sh; dv >>= sh; }
        /* skip transparent top/bottom using spans */
        if (spans && !(cstat & 8) && dv > 0) {
            int32_t top = spans[us], bot = spans[t->w + us];
            if (top >= bot) continue;
            int32_t vtop = top << 16;
            if (v < vtop) {
                int32_t skip = div32(vtop - v + dv - 1, dv);
                ya += skip; v += skip * dv;
            }
            int32_t yend = ya + div32((bot << 16) - v + dv - 1, dv);
            if (yend < yb) yb = yend;
            if (yb <= ya) continue;
        }
        int32_t vmax = t->h << 16;
        if (!(cstat & 8) && dv > 0) {
            if (!spans) {
                if (v < 0) {
                    int32_t skip = div32(-v + dv - 1, dv);
                    ya += skip; v += skip * dv;
                }
                if (v >= vmax) continue;
                int32_t yend = ya + div32(vmax - v + dv - 1, dv);
                if (yend < yb) yb = yend;
                if (yb <= ya) continue;
            }
            pixel_t *dst = frame + ya * pitch + x;
            if (!trans) {
                if (t->flags & DKT_TRANS) {
                    for (int32_t y = ya; y < yb; y++, v += dv, dst += pitch) {
                        uint32_t c = col[v >> 16];
                        if (c != 255) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
                    }
                } else {
                    for (int32_t y = ya; y < yb; y++, v += dv, dst += pitch) {
                        uint32_t c = pal[col[v >> 16]];
                        *dst = (pixel_t)((c << 8) | c);
                    }
                }
            } else {
                int32_t y = ya;
                if ((x ^ y) & 1) { y++; v += dv; dst += pitch; }
                int32_t p2p = pitch << 1, dv2 = dv << 1;
                for (; y < yb; y += 2, v += dv2, dst += p2p) {
                    uint32_t c = col[v >> 16];
                    if (c != 255) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
                }
            }
        } else {
            pixel_t *dst = frame + ya * pitch + x;
            if (!trans) {
                for (int32_t y = ya; y < yb; y++, v += dv, dst += pitch) {
                    if ((uint32_t)v >= (uint32_t)vmax) continue;
                    uint32_t c = col[v >> 16];
                    if (c != 255) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
                }
            } else {
                int32_t y = ya;
                if ((x ^ y) & 1) { y++; v += dv; dst += pitch; }
                int32_t p2p = pitch << 1, dv2 = dv << 1;
                for (; y < yb; y += 2, v += dv2, dst += p2p) {
                    if ((uint32_t)v >= (uint32_t)vmax) continue;
                    uint32_t c = col[v >> 16];
                    if (c != 255) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
                }
            }
        }
    }
    STATS.sprites++;
}

/* wall-aligned sprites (cstat&16): projected as a textured quad column-wise */
static void drawsprite_wall(int32_t snum)
{
    spritetype *tspr = &sprite[tsprite[snum]];
    int16_t gpic = tspr->picnum; uint16_t gcs = tspr->cstat; int8_t gsh = tspr->shade;
    if (!game_tsprite(tsprite[snum], &gpic, &gcs, &gsh)) return;
    int32_t tilenum = picwrap(gpic), cstat = gcs;
    if (PICANM(tilenum) & 192) tilenum = picwrap(tilenum + animofs((int16_t)tilenum, 0));
    const dk_tile_t *t = &tiledir[tilenum];
    if (!(t->flags & DKT_PRESENT) || t->ow <= 0) return;
    const sectortype *sec = &sector[tspr->sectnum];
    int32_t xspan = t->ow, yspan = t->oh;
    int32_t xoff = t->xoff + tspr->xoffset, yoff = t->yoff + tspr->yoffset;
    if (cstat & 4) xoff = -xoff;
    int32_t xv = sintable[tspr->ang & 2047] * tspr->xrepeat;
    int32_t yv = sintable[(tspr->ang + 1536) & 2047] * tspr->xrepeat;
    int32_t k = (xspan >> 1) + xoff;
    int32_t wx1 = tspr->x - mulscale16(xv, k), wy1 = tspr->y - mulscale16(yv, k);
    int32_t wx2 = wx1 + mulscale16(xv, xspan), wy2 = wy1 + mulscale16(yv, xspan);
    /* rotate into view space (Ken's yp = depth*256) */
    int32_t xp1 = dmulscale6(wy1 - posy, cosang, -(wx1 - posx), sinang);
    int32_t yp1 = dmulscale6(wx1 - posx, cosang, wy1 - posy, sinang);
    int32_t xp2 = dmulscale6(wy2 - posy, cosang, -(wx2 - posx), sinang);
    int32_t yp2 = dmulscale6(wx2 - posx, cosang, wy2 - posy, sinang);
    int swapped = 0;
    if (dmulscale32(xp1, yp2, -xp2, yp1) >= 0) {   /* back side */
        if (cstat & 64) return;                     /* one-sided */
        int32_t tt;
        tt = xp1; xp1 = xp2; xp2 = tt; tt = yp1; yp1 = yp2; yp2 = tt;
        swapped = 1;
    }
    if (yp1 < 256 && yp2 < 256) return;
    /* use a temporary scan slot (MAXWALLSB-1) like Ken */
    int32_t z = MAXWALLSB - 1;
    rx1[z] = xp1; ry1[z] = yp1; rx2[z] = xp2; ry2[z] = yp2;
    int32_t bx1, bx2;
    if (xp1 >= -yp1) {
        if (xp1 > yp1 || yp1 <= 0) return;
        bx1 = halfx + scale(xp1, halfx, yp1); if (xp1 >= 0) bx1++;
    } else {
        if (xp2 < -yp2) return;
        bx1 = 0;
    }
    if (xp2 <= yp2) {
        if (xp2 < -yp2 || yp2 <= 0) return;
        bx2 = halfx + scale(xp2, halfx, yp2) - 1; if (xp2 >= 0) bx2++;
    } else {
        if (xp1 > yp1) return;
        bx2 = xdim - 1;
    }
    if (bx1 < cx0) bx1 = cx0;
    if (bx2 > cx1) bx2 = cx1;
    if (bx1 > bx2) return;
    int32_t t1 = wall_t_at(z, bx1), t2 = wall_t_at(z, bx2);
    int32_t d1 = yp1 + mulscale16(t1, yp2 - yp1), d2 = yp1 + mulscale16(t2, yp2 - yp1);
    if (d1 < 256 || d2 < 256) return;
    xb1[z] = (int16_t)bx1; xb2[z] = (int16_t)bx2; yb1[z] = d1; yb2[z] = d2;
    st1[z] = t1; st2[z] = t2;

    int32_t zb = tspr->z, hz = (yspan * tspr->yrepeat) << 2;
    if (cstat & 128) zb += hz >> 1;
    zb -= (yoff * tspr->yrepeat) << 2;
    int32_t zt = zb - hz;
    int32_t yt_a = proj_y16_ni(zt - posz, d1), yt_b = proj_y16_ni(zt - posz, d2);
    int32_t yb_a = proj_y16_ni(zb - posz, d1), yb_b = proj_y16_ni(zb - posz, d2);
    int32_t ytop_min = min(yt_a, yt_b), ybot_max = max(yb_a, yb_b);
    if (ybot_max <= 0 || ytop_min >= (ydim << 16)) return;
    int32_t startum = max(0, ytop_min >> 16);
    int32_t startdm = min(ydim, (ybot_max + 65535) >> 16);
    if (startum >= startdm) return;
    int32_t yp = (d1 + d2) >> 1;
    if (!sprite_clip(tspr, bx1, bx2, yp, startum, startdm, 0, 0)) return;
    int32_t sh = t->shift;
    const uint8_t *pix = (const uint8_t *)DKPTR(t->off);
    const uint8_t *spans = ((t->flags & DKT_SPANS) && !(cstat & 8)) ? pix + t->w * t->h : 0;
    const uint8_t *pal = palptr(tspr->pal, shadeidx(tspr->shade, yp, sectvis(sec)));
    int trans = (cstat & 2) != 0;
    int32_t a1 = rx1[z] >> 4, b1 = ry1[z] >> 4, a2 = rx2[z] >> 4, b2 = ry2[z] >> 4;
    int32_t xc2 = 2 * bx1 + 1 - xdim;
    int64_t num = (int64_t)xc2 * b1 - (int64_t)xdim * a1;
    int64_t den = (int64_t)xdim * (a2 - a1) - (int64_t)xc2 * (b2 - b1);
    int32_t dn = 2 * b1, ddn = -2 * (b2 - b1);
    int32_t ydd = yp2 - yp1;
    int32_t dx_span = bx2 - bx1;
    int can_lerp = (yt_a > -0x3ffe0000 && yt_a < 0x3ffe0000 && yt_b > -0x3ffe0000 && yt_b < 0x3ffe0000 &&
                    yb_a > -0x3ffe0000 && yb_a < 0x3ffe0000 && yb_b > -0x3ffe0000 && yb_b < 0x3ffe0000);
    int32_t yt_cur = yt_a, yt_inc = (can_lerp && dx_span > 0) ? div32(yt_b - yt_a, dx_span) : 0;
    int32_t yb_cur = yb_a, yb_inc = (can_lerp && dx_span > 0) ? div32(yb_b - yb_a, dx_span) : 0;
    int32_t kdv_spr = div64((int64_t)16384 << 18, (int32_t)tspr->yrepeat * ysc);
    for (int32_t x = bx1; x <= bx2; x++, num += dn, den += ddn, yt_cur += yt_inc, yb_cur += yb_inc) {
        int32_t tt;
        if (x == bx1) tt = t1;
        else if (x == bx2) tt = t2;
        else if (!den) tt = 0;
        else {
            int64_t n = num, d0 = den;
            while (d0 > 0x3fffffff || d0 < -0x3fffffff) { d0 >>= 1; n >>= 1; }
            tt = div64(n << 16, (int32_t)d0);
            if (tt < 0) tt = 0; else if (tt > 65536) tt = 65536;
        }
        int32_t d = yp1 + mulscale16(tt, ydd);
        if (d < 256) continue;
        int32_t yt = can_lerp ? ((x == bx2) ? yt_b : yt_cur) : proj_y16_ni(zt - posz, d);
        int32_t ybt = can_lerp ? ((x == bx2) ? yb_b : yb_cur) : proj_y16_ni(zb - posz, d);
        int32_t ya = max((yt + 65535) >> 16, suwall[x]), yb = min((ybt + 65535) >> 16, sdwall[x]);
        if (ya < 0) ya = 0;
        if (yb > ydim) yb = ydim;
        if (yb <= ya || ybt <= yt) continue;
        int32_t u = (int32_t)(mul64(tt, xspan) >> 16);
        if (swapped) u = xspan - 1 - u;
        if (cstat & 4) u = xspan - 1 - u;
        if (u < 0) u = 0;
        if (u >= xspan) u = xspan - 1;
        int32_t dv = (int32_t)(mul64(d, kdv_spr) >> 18);
        if (dv < 1) dv = div64((int64_t)yspan << 32, ybt - yt);
        int32_t v = (int32_t)(mul64((ya << 16) + 32768 - yt, dv) >> 16);
        if (cstat & 8) { v = (yspan << 16) - 1 - v; dv = -dv; }
        if (sh == 1) { v >>= 1; dv >>= 1; u >>= 1; }
        else if (sh) { v >>= sh; dv >>= sh; u >>= sh; }
        if (spans) {
            int32_t top = spans[u], bot = spans[t->w + u];
            if (top >= bot) continue;
            int32_t vtop = top << 16;
            if (v < vtop) {
                int32_t skip = div32(vtop - v + dv - 1, dv);
                ya += skip; v += skip * dv;
            }
            int32_t vbot = bot << 16;
            if (v >= vbot) continue;
            int32_t yend = ya + div32(vbot - v + dv - 1, dv);
            if (yend < yb) yb = yend;
            if (yb <= ya) continue;
        }
        const uint8_t *col = pix + u * t->h;
        int32_t vmax = t->h << 16;
        pixel_t *dst = frame + ya * pitch + x;
        if (!trans) {
            for (int32_t y = ya; y < yb; y++, v += dv, dst += pitch) {
                if ((uint32_t)v >= (uint32_t)vmax) continue;
                uint32_t c = col[v >> 16];
                if (c != 255) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
            }
        } else {
            for (int32_t y = ya; y < yb; y++, v += dv, dst += pitch) {
                if ((uint32_t)v >= (uint32_t)vmax) continue;
                uint32_t c = col[v >> 16];
                if (c != 255 && !((x ^ y) & 1)) { c = pal[c]; *dst = (pixel_t)((c << 8) | c); }
            }
        }
    }
    STATS.sprites++;
}

/* Floor-aligned sprites ((cstat & 48) == 32): Duke builds bridges, grates
 * and lids from them.  The sprite is the rectangle Build's collision code
 * uses (clipmove/getzrange, case 32): corner C0, tile columns (u) along
 * U = (-sin a, cos a), rows (v) along V = (-cos a, -sin a), texel size
 * xrepeat/4 by yrepeat/4.  On screen it is a flat at height z: every row
 * has one depth, so (u, v) are linear across the row and each row's span
 * is solved exactly - only covered pixels are touched.  Clipped against
 * the walls in front with the same smost windows as the other sprites. */
static int64_t fdiv64(int64_t a, int64_t b)
{
    int64_t q;
    if (b == (int32_t)b) {
        q = fast_div64(a, (int32_t)b);
    } else {
        q = a / b;
    }
    int64_t r = a - q * b;
    if (r && ((a < 0) != (b < 0))) q--;
    return q;
}

/* columns k (x = lx + k) with 0 <= c0 + k*dc < lim: narrows [*kmin, *kmax] */
static void span_limit(int64_t c0, int64_t dc, int64_t lim, int64_t *kmin, int64_t *kmax)
{
    int64_t lo, hi;
    if (dc == 0) {
        if (c0 < 0 || c0 >= lim) *kmax = *kmin - 1;
        return;
    }
    if (dc > 0) {                     /* k >= ceil(-c0/dc),  k <= ceil((lim-c0)/dc) - 1 */
        lo = -fdiv64(c0, dc);
        hi = -fdiv64(c0 - lim, dc) - 1;
    } else {                          /* e = -dc: k <= floor(c0/e),  k >= floor((c0-lim)/e) + 1 */
        int64_t e = -dc;
        hi = fdiv64(c0, e);
        lo = fdiv64(c0 - lim, e) + 1;
    }
    if (lo > *kmin) *kmin = lo;
    if (hi < *kmax) *kmax = hi;
}

static void drawsprite_floor(int32_t snum)
{
    spritetype *tspr = &sprite[tsprite[snum]];
    int16_t gpic = tspr->picnum; uint16_t gcs = tspr->cstat; int8_t gsh = tspr->shade;
    if (!game_tsprite(tsprite[snum], &gpic, &gcs, &gsh)) return;
    int32_t tilenum = picwrap(gpic), cstat = gcs;
    if (PICANM(tilenum) & 192) tilenum = picwrap(tilenum + animofs((int16_t)tilenum, 0));
    const dk_tile_t *t = &tiledir[tilenum];
    if (!(t->flags & DKT_PRESENT) || t->ow <= 0 || t->oh <= 0 || !tspr->xrepeat || !tspr->yrepeat) return;
    int32_t z0 = tspr->z - posz;
    if (z0 == 0) return;
    if ((cstat & 64) && ((posz > tspr->z) == ((cstat & 8) == 0))) return;   /* one-sided */
    const sectortype *sec = &sector[tspr->sectnum];

    int32_t xspan = t->ow, yspan = t->oh;
    int32_t xoff = t->xoff + tspr->xoffset, yoff = t->yoff + tspr->yoffset;
    if (cstat & 4) xoff = -xoff;
    if (cstat & 8) yoff = -yoff;
    int32_t ca = sintable[(tspr->ang + 512) & 2047], sa = sintable[tspr->ang & 2047];
    int32_t dax = ((xspan >> 1) + xoff) * tspr->xrepeat, day = ((yspan >> 1) + yoff) * tspr->yrepeat;
    int32_t c0x = tspr->x + dmulscale16(sa, dax, ca, day);
    int32_t c0y = tspr->y + dmulscale16(sa, day, -ca, dax);

    /* Fast 4-corner frustum, band, and vertical row bounds cull */
    int32_t ux = -mulscale16(sa, xspan * tspr->xrepeat), uy = mulscale16(ca, xspan * tspr->xrepeat);
    int32_t vx = -mulscale16(ca, yspan * tspr->yrepeat), vy = -mulscale16(sa, yspan * tspr->yrepeat);
    int32_t cwx[4] = { c0x, c0x + ux, c0x + vx, c0x + ux + vx };
    int32_t cwy[4] = { c0y, c0y + uy, c0y + vy, c0y + uy + vy };
    int32_t all_behind = 1, all_left = 1, all_right = 1, all_front = 1;
    int32_t bx1 = xdim - 1, bx2 = 0, by1 = ydim, by2 = 0;
    for (int k = 0; k < 4; k++) {
        int32_t dx = cwx[k] - posx, dy = cwy[k] - posy;
        int32_t xp = dmulscale6(dy, cosang, -dx, sinang);
        int32_t yp = dmulscale6(dx, cosang, dy, sinang);
        if (yp >= 8) all_behind = 0;
        if (xp >= -yp) all_left = 0;
        if (xp <= yp) all_right = 0;
        if (yp < 256) {
            all_front = 0;
        } else if (all_front) {
            int32_t sx = halfx + scale(xp, halfx, yp);
            int32_t sy = proj_y16_ni(z0, yp) >> 16;
            if (sx - 2 < bx1) bx1 = sx - 2;
            if (sx + 2 > bx2) bx2 = sx + 2;
            if (sy - 2 < by1) by1 = sy - 2;
            if (sy + 2 > by2) by2 = sy + 2;
        }
    }
    if (all_behind || all_left || all_right) return;

    int32_t lx = cx0, rx = cx1;
    int32_t ya = (z0 > 0) ? max(horizon, 0) : 0, yb = (z0 > 0) ? ydim : min(horizon, ydim);
    if (all_front) {
        if (bx2 < cx0 || bx1 > cx1) return;
        if (bx1 > lx) lx = bx1;
        if (bx2 < rx) rx = bx2;
        if (lx > rx) return;
        if (by1 > ya) ya = by1;
        if (by2 < yb) yb = by2;
        if (ya >= yb) return;
    }

    if (!sprite_clip(tspr, lx, rx, spritesy[snum], 0, ydim, 0, 0)) return;
    int32_t min_su = ydim, max_sd = 0;
    for (int32_t x = lx; x <= rx; x++) {
        if (suwall[x] < min_su) min_su = suwall[x];
        if (sdwall[x] > max_sd) max_sd = sdwall[x];
    }
    if (min_su > ya) ya = min_su;
    if (max_sd < yb) yb = max_sd;
    if (ya >= yb) return;

    /* u16(x) = 16/xrepeat * [Au + D*(Bu + Cu*xn)],  xn = (2x+1-xdim)/(2*halfx)
     * (dot products with U, V in 14-bit units; D = row depth in xy units) */
    int64_t ex = posx - c0x, ey = posy - c0y;
    int64_t Au = -ex * sa + ey * ca, Av = -ex * ca - ey * sa;
    int64_t Bu = ((int64_t)sinang * ca - (int64_t)cosang * sa) >> 14;
    int64_t Cu = ((int64_t)sinang * sa + (int64_t)cosang * ca) >> 14;
    int64_t Bv = (-(int64_t)sinang * sa - (int64_t)cosang * ca) >> 14;
    int64_t Cv = ((int64_t)sinang * ca - (int64_t)cosang * sa) >> 14;
    int64_t W16 = (int64_t)xspan << 16, H16 = (int64_t)yspan << 16;

    int32_t sh = t->shift;
    const uint8_t *pix = (const uint8_t *)DKPTR(t->off);
    int trans = (cstat & 2) != 0;
    int32_t den = 2 * halfx;
    int32_t xden = tspr->xrepeat * den, yden = tspr->yrepeat * den;
    int64_t xn0 = 2 * lx + 1 - xdim;
    int32_t vis = sectvis(sec);
    for (int32_t y = ya; y < yb; y++) {
        int32_t dy2a = 2 * y + 1 - 2 * horizon;
        int32_t k16 = (dy2a * k16_6) >> 6;
        if (k16 == 0 || ((k16 > 0) != (z0 > 0))) continue;
        int64_t D = fast_div64((int64_t)z0 << 16, k16);            /* depth, xy units */
        if (D < 8 || D > 0x3fffff) continue;
        /* values at x = lx and the step per column, 16.16 texels */
        int64_t u0 = fast_div64((Au * den + D * (Bu * den + Cu * xn0)) * 16, xden);
        int64_t du = fast_div64(D * Cu * 2 * 16, xden);
        int64_t v0 = fast_div64((Av * den + D * (Bv * den + Cv * xn0)) * 16, yden);
        int64_t dv = fast_div64(D * Cv * 2 * 16, yden);
        int64_t kmin = 0, kmax = rx - lx;
        span_limit(u0, du, W16, &kmin, &kmax);
        span_limit(v0, dv, H16, &kmin, &kmax);
        if (kmin > kmax) continue;
        const uint8_t *palp = palptr(tspr->pal, shadeidx(tspr->shade, (int32_t)(D << 8), vis));
        int32_t u = (int32_t)(u0 + kmin * du), v = (int32_t)(v0 + kmin * dv);
        int32_t idu = (int32_t)du, idv = (int32_t)dv;
        int32_t x0 = lx + (int32_t)kmin, x1 = lx + (int32_t)kmax;
        pixel_t *dst = frame + y * pitch + x0;
        int32_t th = t->h;
        if (sh == 1) {
            for (int32_t x = x0; x <= x1; x++, dst++, u += idu, v += idv) {
                if (y < suwall[x] || y >= sdwall[x]) continue;
                if (trans && ((x ^ y) & 1)) continue;
                int32_t tu = u >> 16, tv = v >> 16;
                if ((uint32_t)tu >= (uint32_t)xspan || (uint32_t)tv >= (uint32_t)yspan) continue;
                if (cstat & 4) tu = xspan - 1 - tu;
                if (cstat & 8) tv = yspan - 1 - tv;
                uint32_t c = pix[(tu >> 1) * th + (tv >> 1)];
                if (c == 255) continue;
                c = palp[c];
                *dst = (pixel_t)((c << 8) | c);
            }
        } else {
            for (int32_t x = x0; x <= x1; x++, dst++, u += idu, v += idv) {
                if (y < suwall[x] || y >= sdwall[x]) continue;
                if (trans && ((x ^ y) & 1)) continue;
                int32_t tu = u >> 16, tv = v >> 16;
                if ((uint32_t)tu >= (uint32_t)xspan || (uint32_t)tv >= (uint32_t)yspan) continue;
                if (cstat & 4) tu = xspan - 1 - tu;
                if (cstat & 8) tv = yspan - 1 - tv;
                uint32_t c = pix[(tu >> sh) * th + (tv >> sh)];
                if (c == 255) continue;
                c = palp[c];
                *dst = (pixel_t)((c << 8) | c);
            }
        }
    }
    STATS.sprites++;
}

static void drawmaskwall(int32_t damaskwallcnt)
{
    int32_t z = maskwall[damaskwallcnt];
    walltype *wal = &wall[thewall[z]];
    int32_t sectnum = thesector[z];
    sectortype *sec = &sector[sectnum];
    int32_t nsecn = wal->nextsector;
    if (nsecn < 0) return;
    sectortype *nsec = &sector[nsecn];
    int32_t i, j, k, x, lx, rx, x1 = xb1[z], x2 = xb2[z];
    int32_t z1 = max(nsec->ceilingz, sec->ceilingz);
    int32_t z2 = min(nsec->floorz, sec->floorz);

    wallmost(uwall, z, sectnum, 0);
    wallmost(uplc, z, nsecn, 0);
    for (x = x1; x <= x2; x++) if (uplc[x] > uwall[x]) uwall[x] = uplc[x];
    wallmost(dwall, z, sectnum, 1);
    wallmost(dplc, z, nsecn, 1);
    for (x = x1; x <= x2; x++) if (dplc[x] < dwall[x]) dwall[x] = dplc[x];

    for (i = smostwallcnt - 1; i >= 0; i--) {
        j = smostwall[i];
        if ((xb1[j] > x2) || (xb2[j] < x1)) continue;
        if (wallfront(j, z)) continue;
        lx = max(xb1[j], x1); rx = min(xb2[j], x2);
        switch (smostwalltype[i]) {
        case 0:
            if (lx <= rx) {
                if ((lx == x1) && (rx == x2)) return;
                for (x = lx; x <= rx; x++) dwall[x] = 0;
            }
            break;
        case 1:
            k = smoststart[i] - xb1[j];
            for (x = lx; x <= rx; x++) if (smost[k + x] > uwall[x]) uwall[x] = smost[k + x];
            break;
        case 2:
            k = smoststart[i] - xb1[j];
            for (x = lx; x <= rx; x++) if (smost[k + x] < dwall[x]) dwall[x] = smost[k + x];
            break;
        }
    }
    for (x = x1; x <= x2; x++) { suwall[x] = umost[x]; sdwall[x] = dmost[x]; umost[x] = 0; dmost[x] = (int16_t)ydim; }
    wallpart_t wp;
    wp.picnum = wal->overpicnum; wp.xpanning = wal->xpanning; wp.ypanning = wal->ypanning;
    wp.shade = wal->shade; wp.pal = wal->pal; wp.cstat = wal->cstat; wp.vis = sectvis(sec);
    wp.yrepeat = wal->yrepeat;
    wp.zrel = posz - ((wal->cstat & 4) ? z2 : z1);
    prepwall(z, wal);
    wallscan(x1, x2, uwall, dwall, &wp, 1);
    for (x = x1; x <= x2; x++) { umost[x] = suwall[x]; dmost[x] = sdwall[x]; }
}

static void drawmasks(void)
{
    int32_t i, j, l, gap, xs, ys, xp, yp;

    for (i = spritesortcnt - 1; i >= 0; i--) {
        spritetype *s = &sprite[tsprite[i]];
        xs = s->x - posx; ys = s->y - posy;
        yp = dmulscale6(xs, cosang, ys, sinang);
        if (yp > (4 << 8)) {
            xp = dmulscale6(ys, cosang, -xs, sinang);
            spritesx[i] = scale(xp + yp, xdim << 7, yp);   /* 24.8 screen x */
        } else if ((s->cstat & 48) == 0) {
            spritesortcnt--;
            if (i != spritesortcnt) {
                tsprite[i] = tsprite[spritesortcnt];
                spritesx[i] = spritesx[spritesortcnt];
                spritesy[i] = spritesy[spritesortcnt];
            }
            continue;
        }
        spritesy[i] = yp;
    }

    gap = 1; while (gap < spritesortcnt) gap = (gap << 1) + 1;
    for (gap >>= 1; gap > 0; gap >>= 1)
        for (i = 0; i < spritesortcnt - gap; i++)
            for (l = i; l >= 0; l -= gap) {
                if (spritesy[l] <= spritesy[l + gap]) break;
                int16_t ts = tsprite[l]; tsprite[l] = tsprite[l + gap]; tsprite[l + gap] = ts;
                int32_t tx = spritesx[l]; spritesx[l] = spritesx[l + gap]; spritesx[l + gap] = tx;
                tx = spritesy[l]; spritesy[l] = spritesy[l + gap]; spritesy[l + gap] = tx;
            }

    /* back to front, interleaving masked walls */
    while ((spritesortcnt > 0) && (maskwallcnt > 0)) {
        j = maskwall[maskwallcnt - 1];
        if (spritesortcnt > 0 && spritesy[spritesortcnt - 1] > ((yb1[j] + yb2[j]) >> 1)) {
            spritesortcnt--;
            spritetype *s = &sprite[tsprite[spritesortcnt]];
            if ((s->cstat & 48) == 16) drawsprite_wall(spritesortcnt);
            else if ((s->cstat & 48) == 0) drawsprite_face(spritesortcnt);
            else if ((s->cstat & 48) == 32) drawsprite_floor(spritesortcnt);
        } else {
            drawmaskwall(--maskwallcnt);
        }
    }
    while (spritesortcnt > 0) {
        spritesortcnt--;
        spritetype *s = &sprite[tsprite[spritesortcnt]];
        if ((s->cstat & 48) == 16) drawsprite_wall(spritesortcnt);
        else if ((s->cstat & 48) == 0) drawsprite_face(spritesortcnt);
        else if ((s->cstat & 48) == 32) drawsprite_floor(spritesortcnt);
    }
    while (maskwallcnt > 0) drawmaskwall(--maskwallcnt);
}

/* --------------------------------------------------------- entry */
void RNAME(render_frame)(int x0, int x1)
{
    int32_t i, closest, j;

    cx0 = x0; cx1 = x1;
    memset(&STATS, 0, sizeof(STATS));
    STATS.mirror_wall = -1;
    posx = rview.posx; posy = rview.posy; posz = rview.posz;
    cosang = sintable[(rview.ang + 512) & 2047];
    sinang = sintable[rview.ang & 2047];
    xdim = rview.xdim; ydim = rview.ydim; halfx = xdim >> 1;
    pitch = rview.pitch; frame = rview.frame;
    fclock = rview.clock;
    viewsprite = rview.viewsprite;
    nightvis = (rview.flags & RVF_NIGHTVISION) != 0;
    /* vertical focal: Build at 4:3 for a xdim x (xdim*0.7) screen */
    int32_t vfocal16 = (halfx * 58720);                 /* 0.896*65536 = 58720 -> 16.16 */
    ysc = vfocal16 >> 12;                               /* vfocal*16 */
    k16_6 = div64((int64_t)128 << 22, ysc);             /* (128*65536 << 6) / ysc */
    horizon = (ydim >> 1) + (int32_t)((mul64(rview.horiz - 100, vfocal16) >> 16) / 128);
    globvis = rview.visibility ? rview.visibility : 512;
    cursect = (int16_t)rview.sectnum;

    for (i = cx0; i <= cx1; i++) { umost[i] = 0; dmost[i] = (int16_t)ydim; }
    memset(gotsector, 0, (numsectors + 7) >> 3);
    numhits = cx1 - cx0 + 1;
    numscans = 0; numbunches = 0;
    maskwallcnt = 0; smostwallcnt = 0; smostcnt = 0; spritesortcnt = 0;
    sky_done_key = 0xffffffffu;
    int16_t ray_dx[RMAXXDIM], ray_dy[RMAXXDIM];
    uint8_t ray_ok = 0;

    if (cursect < 0 || cursect >= numsectors) return;
    RDBG(0x100);
    scansector(cursect);
    RDBG(0x200 + numscans);

    while ((numbunches > 0) && (numhits > 0)) {
        memset(bunchtemp, 0, numbunches);
        bunchtemp[0] = 1;
        closest = 0;
        for (i = 1; i < numbunches; i++) {
            if ((j = bunchfront(i, closest)) < 0) continue;
            bunchtemp[i] = 1;
            if (j == 0) bunchtemp[closest] = 1, closest = i;
        }
        for (i = 0; i < numbunches; i++) {
            if (bunchtemp[i]) continue;
            if ((j = bunchfront(i, closest)) < 0) continue;
            bunchtemp[i] = 1;
            if (j == 0) bunchtemp[closest] = 1, closest = i, i = 0;
        }
        RDBG(0x400 + STATS.bunches);
        drawalls(closest, ray_dx, ray_dy, &ray_ok);
        RDBG(0x800 + STATS.bunches);
        STATS.bunches++;
        numbunches--;
        bunchfirst[closest] = bunchfirst[numbunches];
        bunchlast[closest] = bunchlast[numbunches];
    }
    RDBG(0xA00);
    drawmasks();
    RDBG(0xB00);
}
