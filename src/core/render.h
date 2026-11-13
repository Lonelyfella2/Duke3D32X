/*
 * render.h - Build-style sector/portal renderer (shared interface).
 *
 * The renderer is compiled twice (render_m.c / render_s.c) so each SH-2 gets
 * private state; each renders a column band [x0,x1] of the same view.
 * Pixels are 16-bit "doubled" 8bpp values ((c<<8)|c): one internal pixel is
 * two 32X framebuffer bytes, and the line table doubles rows for free.
 */
#ifndef RENDER_H
#define RENDER_H

#include "engine.h"

typedef uint16_t pixel_t;

#define MIRROR 560          /* Duke tile MIRROR (NAMES.H) */
#define RMAXXDIM 160
#define RMAXYDIM 128

typedef struct {
    int32_t posx, posy, posz;
    int32_t ang;            /* 0..2047                                  */
    int32_t horiz;          /* Build horiz, 100 = level                 */
    int32_t sectnum;
    int32_t xdim, ydim;     /* internal view size (<= RMAXXDIM/YDIM)    */
    pixel_t *frame;         /* top-left of the 3D view                  */
    int32_t pitch;          /* pixel_t units per row                    */
    int32_t visibility;     /* global visibility (Duke default 512)     */
    int32_t clock;          /* totalclock snapshot for animations       */
    int32_t viewsprite;     /* sprite hidden from the view (player)     */
    int32_t skypicnum;      /* parallax base tile                       */
    int32_t parallaxyscale; /* Duke setupbackdrop() value               */
    int8_t  pskyoff[8];
    int32_t mirrorwall;     /* reserved                                 */
    int32_t flags;          /* RVF_*                                    */
    int32_t frameno;
    int32_t pad[3];
} rview_t;

#define RVF_NIGHTVISION 1   /* draw everything at shade 0 (heat goggles) */
#define RVF_MIRRORPASS  2   /* rendering the reflected view              */

extern rview_t rview;

/* per-frame statistics, written by each renderer instance */
typedef struct {
    int32_t walls, bunches, sprites, spans, cols;
    int32_t mirror_wall, mirror_x1, mirror_x2;     /* MIRROR seen this frame */
} rstats_t;

void render_init(void);
void render_frame_m(int x0, int x1);
void render_frame_s(int x0, int x1);
void render_mirror(const rstats_t *m);     /* master: reflected pass + flip */
void render_merge_mirror(rstats_t *out, const rstats_t *a, const rstats_t *b);
extern rstats_t rstats_m, rstats_s;
extern int16_t skyangle[RMAXXDIM];
extern int32_t skyangle_width;

/* gameplay hook: per-sprite picture/flags for this view (read-only, called
 * by both CPUs); return 0 to skip the sprite. */
int  game_tsprite(int snum, int16_t *picnum, uint16_t *cstat, int8_t *shade);

/* 2D drawing helpers (hud.c) */
static inline pixel_t dblpix(uint32_t c) { return (pixel_t)((c << 8) | c); }

#endif
