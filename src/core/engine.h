/*
 * engine.h - Build-engine style world state for the Duke Nukem 3D 32X port.
 *
 * Geometry/collision functions are Ken Silverman's (build_engine.c, used
 * under the Build license); the renderer is reimplemented in render_impl.c.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include "compat.h"
#include "dukedata.h"

/* Reduced limits: E1L4 has 557 sectors and 3437 walls. The converter caps
 * its map sprites at 912, leaving 104 slots for the player and runtime spawns.
 * The link-time memory check guards the 32X SDRAM/stack boundary. */
#define MAXSECTORS   600
#define MAXWALLS     3440
#define MAXSPRITES   1016
#define MAXSTATUS    32
#define MAXTILES     6144
#define MAXPLAYERS   1

#define CLIPMASK0 (((1L) << 16) + 1L)
#define CLIPMASK1 (((256L) << 16) + 64L)

extern sectortype sector[MAXSECTORS];
extern walltype wall[MAXWALLS];
extern spritetype sprite[MAXSPRITES];
extern const dk_wallx_t *wallx;            /* ROM: nextwall, lotag, hitag */
extern dk_wallx_t wallx_dyn_dummy;
extern int32_t numsectors, numwalls;

extern int16_t headspritesect[MAXSECTORS + 1], headspritestat[MAXSTATUS + 1];
extern int16_t prevspritesect[MAXSPRITES], nextspritesect[MAXSPRITES];
extern int16_t prevspritestat[MAXSPRITES], nextspritestat[MAXSPRITES];

/* --------------------------------------------------------------- data */
extern const uint8_t *dk_blob;
extern const dk_header_t *dk_hdr;
extern const dk_tile_t *tiledir;
extern int32_t numtiles;
extern const int16_t *sintable;
extern const int16_t *radarang;
extern const uint8_t *palookup[DK_MAXPALS];
extern const uint16_t *dk_cram;           /* 256 CRAM entries */
extern const uint8_t *textfont, *smalltextfont;

#define DKPTR(off) ((const void *)(dk_blob + (off)))

static inline int32_t TILESIZX(int32_t p) { return ((uint32_t)p < (uint32_t)numtiles) ? tiledir[p].ow : 0; }
static inline int32_t TILESIZY(int32_t p) { return ((uint32_t)p < (uint32_t)numtiles) ? tiledir[p].oh : 0; }
static inline int32_t PICANM(int32_t p)
{
    if ((uint32_t)p >= (uint32_t)numtiles) return 0;
    const dk_tile_t *t = &tiledir[p];
    return (int32_t)((uint32_t)t->anim | ((uint32_t)(uint8_t)t->xoff << 8) |
                     ((uint32_t)(uint8_t)t->yoff << 16) | ((uint32_t)(t->speed & 15) << 24));
}
static inline int tile_present(int32_t p)
{
    return (uint32_t)p < (uint32_t)numtiles && (tiledir[p].flags & DKT_PRESENT);
}

/* -------------------------------------------------------------- clock */
extern volatile int32_t totalclock;        /* 120 Hz like Build */
extern int32_t totalclocklock;
extern int32_t randomseed;
extern int16_t editstatus;

/* ------------------------------------------------------- clip globals */
#define MAXCLIPNUM 128
#define MAXCLIPDIST 1024
typedef struct { int32_t x1, y1, x2, y2; } linetype;
extern int16_t clipnum, hitwalls[4];
extern int32_t hitscangoalx, hitscangoaly;
extern linetype clipit[MAXCLIPNUM];
extern int16_t clipsectorlist[MAXCLIPNUM], clipsectnum;
extern int16_t clipobjectval[MAXCLIPNUM];
extern int32_t rxi[8], ryi[8], rzi[8], rxi2[8], ryi2[8], rzi2[8];
extern int32_t clipmoveboxtracenum;
extern const uint8_t pow2char[8];
extern const int32_t pow2long[32];

#define addclipline(dax1, day1, dax2, day2, daoval)            \
{                                                              \
    if (clipnum < MAXCLIPNUM) {                                \
    clipit[clipnum].x1 = dax1; clipit[clipnum].y1 = day1;      \
    clipit[clipnum].x2 = dax2; clipit[clipnum].y2 = day2;      \
    clipobjectval[clipnum] = daoval;                           \
    clipnum++; }                                               \
}

/* ---------------------------------------------------------- functions */
int32_t ksqrt(uint32_t num);
static inline int32_t krecip(int32_t num) { return num ? (int32_t)((1LL << 30) / num) : 0x7fffffff; }

int  engine_init(const void *blob);
int  engine_loadboard(int mapnum, int32_t *px, int32_t *py, int32_t *pz, int16_t *pang, int16_t *psect);
const dk_map_t *engine_mapinfo(int mapnum);
int  engine_nummaps(void);
const char *engine_quote(int q);
const dk_pic_t *engine_pic(int n);          /* full-screen picture or NULL */

int32_t clipinsidebox(int32_t x, int32_t y, int16_t wallnum, int32_t walldist);
int32_t clipinsideboxline(int32_t x, int32_t y, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t walldist);
int32_t inside(int32_t x, int32_t y, int16_t sectnum);
int32_t getangle(int32_t xvect, int32_t yvect);
int32_t setsprite(int16_t spritenum, int32_t newx, int32_t newy, int32_t newz);
int32_t animateoffs(int16_t tilenum, int16_t fakevar);
void    initspritelists(void);
int32_t insertsprite(int16_t sectnum, int16_t statnum);
int32_t insertspritesect(int16_t sectnum);
int32_t insertspritestat(int16_t statnum);
int32_t deletesprite(int16_t spritenum);
int32_t deletespritesect(int16_t deleteme);
int32_t deletespritestat(int16_t deleteme);
int32_t changespritesect(int16_t spritenum, int16_t newsectnum);
int32_t changespritestat(int16_t spritenum, int16_t newstatnum);
int32_t nextsectorneighborz(int16_t sectnum, int32_t thez, int16_t topbottom, int16_t direction);
int32_t cansee(int32_t x1, int32_t y1, int32_t z1, int16_t sect1, int32_t x2, int32_t y2, int32_t z2, int16_t sect2);
int32_t hitscan(int32_t xs, int32_t ys, int32_t zs, int16_t sectnum, int32_t vx, int32_t vy, int32_t vz,
                int16_t *hitsect, int16_t *hitwall, int16_t *hitsprite,
                int32_t *hitx, int32_t *hity, int32_t *hitz, uint32_t cliptype);
int32_t neartag(int32_t xs, int32_t ys, int32_t zs, int16_t sectnum, int16_t ange, int16_t *neartagsector,
                int16_t *neartagwall, int16_t *neartagsprite, int32_t *neartaghitdist,
                int32_t neartagrange, char tagsearch);
int32_t lintersect(int32_t x1, int32_t y1, int32_t z1, int32_t x2, int32_t y2, int32_t z2, int32_t x3,
                   int32_t y3, int32_t x4, int32_t y4, int32_t *intx, int32_t *inty, int32_t *intz);
int32_t rintersect(int32_t x1, int32_t y1, int32_t z1, int32_t vx, int32_t vy, int32_t vz, int32_t x3,
                   int32_t y3, int32_t x4, int32_t y4, int32_t *intx, int32_t *inty, int32_t *intz);
void    dragpoint(int16_t pointhighlight, int32_t dax, int32_t day);
int32_t lastwall(int16_t point);
int32_t clipmove(int32_t *x, int32_t *y, int32_t *z, int16_t *sectnum, int32_t xvect, int32_t yvect,
                 int32_t walldist, int32_t ceildist, int32_t flordist, uint32_t cliptype);
void    keepaway(int32_t *x, int32_t *y, int32_t w);
int32_t raytrace(int32_t x3, int32_t y3, int32_t *x4, int32_t *y4);
int32_t pushmove(int32_t *x, int32_t *y, int32_t *z, int16_t *sectnum, int32_t walldist,
                 int32_t ceildist, int32_t flordist, uint32_t cliptype);
void    updatesector(int32_t x, int32_t y, int16_t *sectnum);
void    rotatepoint(int32_t xpivot, int32_t ypivot, int32_t x, int32_t y, int16_t daang, int32_t *x2, int32_t *y2);
int32_t krand(void);
void    getzrange(int32_t x, int32_t y, int32_t z, int16_t sectnum, int32_t *ceilz, int32_t *ceilhit,
                  int32_t *florz, int32_t *florhit, int32_t walldist, uint32_t cliptype);
int32_t sectorofwall(int16_t theline);
int32_t getceilzofslope(int16_t sectnum, int32_t dax, int32_t day);
int32_t getflorzofslope(int16_t sectnum, int32_t dax, int32_t day);
void    getzsofslope(int16_t sectnum, int32_t dax, int32_t day, int32_t *ceilz, int32_t *florz);

#endif
