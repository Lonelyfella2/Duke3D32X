/* render_common.c - state shared by both renderer instances */
#include "render.h"

rview_t rview;
int16_t skyangle[RMAXXDIM];
int32_t skyangle_width;

void render_init(void)
{
    memset(&rview, 0, sizeof(rview));
    rview.visibility = 512;
    rview.parallaxyscale = 32768;
    rview.viewsprite = -1;
}

__attribute__((weak)) int game_tsprite(int snum, int16_t *picnum, uint16_t *cstat, int8_t *shade)
{
    (void)snum; (void)picnum; (void)cstat; (void)shade;
    return 1;
}

/* Duke/Build mirrors (GAME.C displayrooms + ENGINE.C preparemirror /
 * completemirror): render from the camera reflected across the mirror
 * wall, starting in the mirror sector behind it, into the flipped column
 * band, then copy those columns back mirrored.  The main pass then draws
 * the MIRROR wall as see-through glass over this image. */
void render_mirror(const rstats_t *m)
{
    static pixel_t row[RMAXXDIM];
    if (m->mirror_wall < 0 || m->mirror_wall >= numwalls) return;
    const walltype *w = &wall[m->mirror_wall], *w2 = &wall[w->point2];
    int sect = w->nextsector;
    if (sect < 0) return;
    int32_t x = w->x, dx = w2->x - x, y = w->y, dy = w2->y - y;
    int64_t j = (int64_t)dx * dx + (int64_t)dy * dy;
    if (j == 0) return;
    int64_t i = (((int64_t)(rview.posx - x) * dx + (int64_t)(rview.posy - y) * dy) << 1);
    rview_t save = rview;
    rview.posx = (x << 1) + (int32_t)((dx * i) / j) - save.posx;
    rview.posy = (y << 1) + (int32_t)((dy * i) / j) - save.posy;
    rview.ang = ((getangle(dx, dy) << 1) - save.ang) & 2047;
    rview.sectnum = sect;
    rview.flags |= RVF_MIRRORPASS;
    rview.viewsprite = -1;                       /* Duke sees himself */
    int me = save.viewsprite;
    uint16_t mecs = 0;
    if (me >= 0 && me < MAXSPRITES) { mecs = sprite[me].cstat; sprite[me].cstat &= ~0x8000; }
    int xd = rview.xdim;
    int fx1 = xd - 1 - m->mirror_x2, fx2 = xd - 1 - m->mirror_x1;
    if (fx1 < 0) fx1 = 0;
    if (fx2 > xd - 1) fx2 = xd - 1;
    if (fx1 <= fx2) {
        render_frame_m(fx1, fx2);
        for (int yy = 0; yy < rview.ydim; yy++) {
            pixel_t *r = rview.frame + yy * rview.pitch;
            for (int xx = fx1; xx <= fx2; xx++) row[xx] = r[xx];
            for (int xx = m->mirror_x1; xx <= m->mirror_x2; xx++) r[xx] = row[xd - 1 - xx];
        }
    }
    if (me >= 0 && me < MAXSPRITES) sprite[me].cstat = mecs;
    rview = save;
}

/* merge both CPUs' mirror sightings into *out */
void render_merge_mirror(rstats_t *out, const rstats_t *a, const rstats_t *b)
{
    out->mirror_wall = -1;
    const rstats_t *src[2] = { a, b };
    for (int k = 0; k < 2; k++) {
        if (src[k]->mirror_wall < 0) continue;
        if (out->mirror_wall < 0) { *out = *src[k]; continue; }
        if (src[k]->mirror_x1 < out->mirror_x1) out->mirror_x1 = src[k]->mirror_x1;
        if (src[k]->mirror_x2 > out->mirror_x2) out->mirror_x2 = src[k]->mirror_x2;
    }
}
