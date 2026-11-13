/*
 * g_sector.c - doors, lifts, switches, activators and sector effectors.
 * Behaviour follows SECTOR.C / ACTORS.C of the GPL Duke Nukem 3D source.
 */
#include "g_local.h"

#define MAXANIMATES 64
static int32_t *animateptr[MAXANIMATES];
static int32_t animategoal[MAXANIMATES], animatevel[MAXANIMATES];
static int16_t animatesect[MAXANIMATES];
static int animatecnt;

#define MAXMSPOINTS 1024
static int16_t msx[MAXMSPOINTS], msy[MAXMSPOINTS];
static int msnum;

#define T1(i) act[i].t[0]
#define T2(i) act[i].t[1]
#define T3(i) act[i].t[2]
#define T4(i) act[i].t3
#define T5(i) act[i].timer
#define T6(i) act[i].extra2


static int activatewarpelevators(int s, int d);

int isanunderoperator(int lotag)
{
    switch (lotag & 0xff) { case 15: case 16: case 17: case 18: case 19: case 22: case 26: return 1; }
    return 0;
}

int isanearoperator(int lotag)
{
    switch (lotag & 0xff) {
    case 9: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
    case 25: case 26: case 29: return 1;
    }
    return 0;
}

/* --------------------------------------------------------- animations */
int getanimationgoal(int32_t *animptr)
{
    for (int i = animatecnt - 1; i >= 0; i--) if (animptr == animateptr[i]) return i;
    return -1;
}

int setanimation(int animsect, int32_t *animptr, int32_t thegoal, int32_t thevel)
{
    int i, j;
    if (animatecnt >= MAXANIMATES - 1) return -1;
    j = animatecnt;
    for (i = 0; i < animatecnt; i++) if (animptr == animateptr[i]) { j = i; break; }
    animatesect[j] = (int16_t)animsect;
    animateptr[j] = animptr;
    animategoal[j] = thegoal;
    animatevel[j] = (thegoal >= *animptr) ? thevel : -thevel;
    if (j == animatecnt) animatecnt++;
    return j;
}

void doanimations(void)
{
    for (int i = animatecnt - 1; i >= 0; i--) {
        int32_t a = *animateptr[i], v = animatevel[i] * TICSPERFRAME;
        int dasect = animatesect[i];
        if (a == animategoal[i]) {
            animatecnt--;
            animateptr[i] = animateptr[animatecnt];
            animategoal[i] = animategoal[animatecnt];
            animatevel[i] = animatevel[animatecnt];
            animatesect[i] = animatesect[animatecnt];
            if ((sector[dasect].lotag == 18 || sector[dasect].lotag == 19) && animateptr[i] == &sector[dasect].ceilingz)
                continue;
            if ((sector[dasect].lotag & 0xff) != 22) callsound(dasect, -1);
            continue;
        }
        if (v > 0) a = min(a + v, animategoal[i]); else a = max(a + v, animategoal[i]);
        if (animateptr[i] == &sector[dasect].floorz) {
            if (pl.cursectnum == dasect && (sector[dasect].floorz - pl.posz) < (64 << 8)) {
                pl.posz += a - *animateptr[i];
                pl.poszv = 0;
            }
            for (int j = headspritesect[dasect]; j >= 0; j = nextspritesect[j])
                if (sprite[j].statnum != STAT_EFFECTOR) sprite[j].z += a - *animateptr[i];
        }
        *animateptr[i] = a;
    }
}

/* MUSICANDSFX in a sector = its door/lift sounds (lotag start, hitag stop) */
void callsound(int sn, int whatsprite)
{
    for (int i = headspritesect[sn]; i >= 0; i = nextspritesect[i]) {
        spritetype *s = &sprite[i];
        if (s->picnum == MUSICANDSFX && s->lotag < 1000) {
            if (whatsprite == -1) whatsprite = i;
            if (T1(i) == 0) {
                if (s->lotag) spritesound(s->lotag, whatsprite);
                if ((sector[s->sectnum].lotag & 0xff) != 22) T1(i) = 1;
            } else {
                if (s->hitag) spritesound(s->hitag, whatsprite);
                T1(i) = 0;
            }
            return;
        }
    }
}

/* -------------------------------------------------- moving sectors */
static void ms(int i)
{
    spritetype *s = &sprite[i];
    s->x += (s->xvel * (sintable[(s->ang + 512) & 2047])) >> 14;
    s->y += (s->xvel * (sintable[s->ang & 2047])) >> 14;
    int j = T2(i);
    int k = T3(i);
    int startwall = sector[s->sectnum].wallptr, endwall = startwall + sector[s->sectnum].wallnum;
    for (int x = startwall; x < endwall && j < MAXMSPOINTS; x++, j++) {
        int32_t tx, ty;
        rotatepoint(0, 0, msx[j], msy[j], (int16_t)(k & 2047), &tx, &ty);
        dragpoint((int16_t)x, s->x + tx, s->y + ty);
    }
}

static void record_ms(int i)
{
    spritetype *sp = &sprite[i];
    int sect = sp->sectnum;
    int startwall = sector[sect].wallptr, endwall = startwall + sector[sect].wallnum;
    T2(i) = msnum;
    for (int s = startwall; s < endwall; s++) {
        if (msnum >= MAXMSPOINTS) break;
        msx[msnum] = (int16_t)(wall[s].x - sp->x);
        msy[msnum] = (int16_t)(wall[s].y - sp->y);
        msnum++;
    }
}

/* ------------------------------------------------------ operatesectors */
void operatesectors(int sn, int ii)
{
    int32_t j = 0, l, q, i;
    sectortype *sptr = &sector[sn];

    switch (sptr->lotag & (0xffff - 49152)) {
    case 9: {               /* star-trek sliding doors (two panels) */
        int startwall = sptr->wallptr, endwall = startwall + sptr->wallnum - 1;
        int32_t sp = sptr->extra >> 4, dax = 0, day = 0, dax2, day2;
        int wallfind[2] = { -1, -1 };
        for (i = startwall; i <= endwall; i++) { dax += wall[i].x; day += wall[i].y; }
        dax /= (endwall - startwall + 1);
        day /= (endwall - startwall + 1);
        for (i = startwall; i <= endwall; i++)
            if (wall[i].x == dax || wall[i].y == day) {
                if (wallfind[0] == -1) wallfind[0] = i; else wallfind[1] = i;
            }
        for (j = 0; j < 2; j++) {
            int wf = wallfind[j];
            if (wf < 0) continue;
            i = wf - 1; if (i < startwall) i = endwall;
            int p2 = wall[wf].point2;
            dax2 = ((wall[i].x + wall[p2].x) >> 1) - wall[wf].x;
            day2 = ((wall[i].y + wall[p2].y) >> 1) - wall[wf].y;
            if (wall[wf].x == dax && wall[wf].y == day) {
                if (dax2 != 0) {
                    dax2 = wall[wall[p2].point2].x - wall[p2].x;
                    setanimation(sn, &wall[wf].x, wall[wf].x + dax2, sp);
                    setanimation(sn, &wall[i].x, wall[i].x + dax2, sp);
                    setanimation(sn, &wall[p2].x, wall[p2].x + dax2, sp);
                    callsound(sn, ii);
                } else if (day2 != 0) {
                    day2 = wall[wall[p2].point2].y - wall[p2].y;
                    setanimation(sn, &wall[wf].y, wall[wf].y + day2, sp);
                    setanimation(sn, &wall[i].y, wall[i].y + day2, sp);
                    setanimation(sn, &wall[p2].y, wall[p2].y + day2, sp);
                    callsound(sn, ii);
                }
            } else {
                if (dax2 != 0) {
                    setanimation(sn, &wall[wf].x, dax, sp);
                    setanimation(sn, &wall[i].x, dax + dax2, sp);
                    setanimation(sn, &wall[p2].x, dax + dax2, sp);
                    callsound(sn, ii);
                } else if (day2 != 0) {
                    setanimation(sn, &wall[wf].y, day, sp);
                    setanimation(sn, &wall[i].y, day + day2, sp);
                    setanimation(sn, &wall[p2].y, day + day2, sp);
                    callsound(sn, ii);
                }
            }
        }
        return;
    }
    case 16: case 17:
        if (getanimationgoal(&sptr->floorz) == -1) {
            i = nextsectorneighborz((int16_t)sn, sptr->floorz, 1, 1);
            if (i == -1) { i = nextsectorneighborz((int16_t)sn, sptr->floorz, 1, -1); if (i == -1) return; }
            setanimation(sn, &sptr->floorz, sector[i].floorz, sptr->extra);
            callsound(sn, ii);
        }
        return;
    case 18: case 19:
        if (getanimationgoal(&sptr->floorz) == -1) {
            i = nextsectorneighborz((int16_t)sn, sptr->floorz, 1, -1);
            if (i == -1) i = nextsectorneighborz((int16_t)sn, sptr->floorz, 1, 1);
            if (i == -1) return;
            j = sector[i].floorz;
            /* Red Light District's opening car starts high above its exit.
             * Its decorative side sectors are 24 units above the actual
             * landing, so the generic nearest-floor rule stops it short.
             * Only the initial descent targets the exit directly; later
             * operations keep the normal neighboring-floor behavior. */
            if (level_index == 1 && sn == 201 && sptr->floorz == -57344)
                j = sector[200].floorz;
            q = sptr->extra;
            l = sptr->ceilingz - sptr->floorz;
            setanimation(sn, &sptr->floorz, j, q);
            setanimation(sn, &sptr->ceilingz, j + l, q);
            callsound(sn, ii);
        }
        return;
    case 29:
        if (sptr->lotag & 0x8000) j = sector[nextsectorneighborz((int16_t)sn, sptr->ceilingz, 1, 1)].floorz;
        else j = sector[nextsectorneighborz((int16_t)sn, sptr->ceilingz, -1, -1)].ceilingz;
        sptr->lotag ^= 0x8000;
        setanimation(sn, &sptr->ceilingz, j, sptr->extra);
        callsound(sn, ii);
        return;
    case 20:
REDODOOR:
        if (sptr->lotag & 0x8000) {
            for (i = headspritesect[sn]; i >= 0; i = nextspritesect[i])
                if (sprite[i].statnum == STAT_EFFECTOR && sprite[i].lotag == 9) { j = sprite[i].z; break; }
            if (i == -1) j = sptr->floorz;
        } else {
            j = nextsectorneighborz((int16_t)sn, sptr->ceilingz, -1, -1);
            if (j >= 0) j = sector[j].ceilingz;
            else { sptr->lotag |= 32768; goto REDODOOR; }
        }
        sptr->lotag ^= 0x8000;
        setanimation(sn, &sptr->ceilingz, j, sptr->extra);
        callsound(sn, ii);
        return;
    case 21:
        i = getanimationgoal(&sptr->floorz);
        if (i >= 0) {
            if (animategoal[i] == sptr->ceilingz)
                animategoal[i] = sector[nextsectorneighborz((int16_t)sn, sptr->ceilingz, 1, 1)].floorz;
            else animategoal[i] = sptr->ceilingz;
        } else {
            if (sptr->ceilingz == sptr->floorz) j = sector[nextsectorneighborz((int16_t)sn, sptr->ceilingz, 1, 1)].floorz;
            else j = sptr->ceilingz;
            sptr->lotag ^= 0x8000;
            if (setanimation(sn, &sptr->floorz, j, sptr->extra) >= 0) callsound(sn, ii);
        }
        return;
    case 22:
        if (sptr->lotag & 0x8000) {
            q = (sptr->ceilingz + sptr->floorz) >> 1;
            setanimation(sn, &sptr->floorz, q, sptr->extra);
            setanimation(sn, &sptr->ceilingz, q, sptr->extra);
        } else {
            int a = nextsectorneighborz((int16_t)sn, sptr->floorz, 1, 1);
            int b = nextsectorneighborz((int16_t)sn, sptr->ceilingz, -1, -1);
            if (a >= 0) setanimation(sn, &sptr->floorz, sector[a].floorz, sptr->extra);
            if (b >= 0) setanimation(sn, &sptr->ceilingz, sector[b].ceilingz, sptr->extra);
        }
        sptr->lotag ^= 0x8000;
        callsound(sn, ii);
        return;
    case 23: {              /* swinging doors: SE 11 effectors with the same hitag */
        int jj = -1;
        q = 0;
        for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
            if (sprite[i].lotag == 11 && sprite[i].sectnum == sn && !T5(i)) { jj = i; break; }
        if (jj < 0) return;
        l = sector[sprite[jj].sectnum].lotag & 0x8000;
        for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
            if (l == (sector[sprite[i].sectnum].lotag & 0x8000) && sprite[i].lotag == 11 &&
                sprite[jj].hitag == sprite[i].hitag && !T5(i)) {
                if (sector[sprite[i].sectnum].lotag & 0x8000) sector[sprite[i].sectnum].lotag &= 0x7fff;
                else sector[sprite[i].sectnum].lotag |= 0x8000;
                T5(i) = 1;
                T4(i) = -T4(i);
                if (q == 0) { callsound(sn, i); q = 1; }
            }
        return;
    }
    case 25: {              /* subway-type sliding doors: SE 15 */
        int jj;
        for (jj = headspritestat[STAT_EFFECTOR]; jj >= 0; jj = nextspritestat[jj])
            if (sprite[jj].lotag == 15 && sprite[jj].sectnum == sn) break;
        if (jj < 0) return;
        for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
            if (sprite[i].hitag == sprite[jj].hitag && sprite[i].lotag == 15) {
                sector[sprite[i].sectnum].lotag ^= 0x8000;
                sprite[i].ang += 1024;
                callsound(sprite[i].sectnum, i);
                T5(i) = (sector[sprite[i].sectnum].lotag & 0x8000) ? 1 : 2;
            }
        return;
    }
    case 26: {
        if (getanimationgoal(&sptr->ceilingz) == -1) {
            sptr->lotag = (sptr->lotag & 0xff00) | 22; operatesectors(sn, ii);
            sptr->lotag = (sptr->lotag & 0xff00) | 9;  operatesectors(sn, ii);
            sptr->lotag = (sptr->lotag & 0xff00) | 26;
        }
        return;
    }
    case 15: {            /* warping elevators (SE 17) - SECTOR.C */
        if (ii != pl.i) return;                 /* only the player rides them */
        for (i = headspritesect[sn]; i >= 0; i = nextspritesect[i])
            if (sprite[i].picnum == SECTOREFFECTOR && sprite[i].lotag == 17) break;
        if (i < 0) return;
        if (pl.cursectnum == sn) {              /* inside the car: go to the other floor */
            if (activatewarpelevators(i, -1)) activatewarpelevators(i, 1);
            else if (activatewarpelevators(i, 1)) activatewarpelevators(i, -1);
        } else {                                /* calling it from the landing */
            if (sptr->floorz > pl.posz + PHEIGHT) activatewarpelevators(i, -1);
            else activatewarpelevators(i, 1);
        }
        return;
    }
    case 27: case 28:     /* stretch / drop - treated as floor doors */
        return;
    case 30:
        j = sector[sn].hitag;
        if (j >= 0 && j < MAXSPRITES) sprite[j].extra = (sprite[j].extra == 1) ? 3 : 1;
        callsound(sn, ii);
        return;
    case 31:
        j = sector[sn].hitag;
        if (j >= 0 && j < MAXSPRITES && T5(j) == 0) T5(j) = 1;
        callsound(sn, ii);
        return;
    }
}

/* ------------------------------------------------------ activators */
void operateactivators(int low, int snum)
{
    int i, j, k;
    (void)snum;
    for (i = headspritestat[STAT_ACTIVATOR]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        if (s->lotag != low) continue;
        if (s->picnum == ACTIVATORLOCKED) {
            sector[s->sectnum].lotag ^= 16384;
            player_quote(sector[s->sectnum].lotag & 16384 ? 4 : 8);
        } else {
            switch (s->hitag) {
            case 0: break;
            case 1: if (sector[s->sectnum].floorz != sector[s->sectnum].ceilingz) continue; break;
            case 2: if (sector[s->sectnum].floorz == sector[s->sectnum].ceilingz) continue; break;
            }
            if (sector[s->sectnum].lotag < 3) {
                for (j = headspritesect[s->sectnum]; j >= 0; j = nextspritesect[j])
                    if (sprite[j].statnum == STAT_EFFECTOR) switch (sprite[j].lotag) {
                        case 36: case 31: case 32: case 18:
                            T1(j) = 1 - T1(j);
                            callsound(s->sectnum, j);
                            break;
                    }
            }
            operatesectors(s->sectnum, i);
        }
    }
    /* effectors controlled directly by activator lotag (lights, rotators) */
    for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        if (s->hitag != low) continue;
        switch (s->lotag) {
        case 12: T1(i) = (T1(i) == 1) ? 2 : 1; break;         /* light switch */
        case 1:  T1(i) = !T1(i); break;                         /* rotator pivot */
        case 4: case 3: T5(i) = !T5(i); break;
        case 2: if (T1(i) == 0 && T5(i) == 0) T5(i) = 1; break;  /* earthquake */
        }
    }
    operaterespawns(low);          /* SECTOR.C: RESPAWN sprites with this lotag */
}

void operatemasterswitches(int low)
{
    for (int i = headspritestat[STAT_STANDABLE]; i >= 0; i = nextspritestat[i])
        if (sprite[i].picnum == MASTERSWITCH && sprite[i].lotag == low && sprite[i].yvel == 0)
            sprite[i].yvel = 1;
}

void activatebysector(int sect, int j)
{
    int didit = 0;
    for (int i = headspritesect[sect]; i >= 0; i = nextspritesect[i])
        if (sprite[i].picnum == ACTIVATOR) { operateactivators(sprite[i].lotag, -1); didit = 1; }
    if (!didit) operatesectors(sect, j);
}

/* ----------------------------------------------------------- switches */
static int switch_base(int pic)
{
    switch (pic) {
    case DIPSWITCH: case DIPSWITCH + 1: return DIPSWITCH;
    case TECHSWITCH: case TECHSWITCH + 1: return TECHSWITCH;
    case ALIENSWITCH: case ALIENSWITCH + 1: return ALIENSWITCH;
    case DIPSWITCH2: case DIPSWITCH2 + 1: return DIPSWITCH2;
    case DIPSWITCH3: case DIPSWITCH3 + 1: return DIPSWITCH3;
    case LIGHTSWITCH: case LIGHTSWITCH + 1: return LIGHTSWITCH;
    case LIGHTSWITCH2: case LIGHTSWITCH2 + 1: return LIGHTSWITCH2;
    case POWERSWITCH1: case POWERSWITCH1 + 1: return POWERSWITCH1;
    case POWERSWITCH2: case POWERSWITCH2 + 1: return POWERSWITCH2;
    case LOCKSWITCH1: case LOCKSWITCH1 + 1: return LOCKSWITCH1;
    case PULLSWITCH: case PULLSWITCH + 1: return PULLSWITCH;
    case HANDSWITCH: case HANDSWITCH + 1: return HANDSWITCH;
    case SPACEDOORSWITCH: case SPACEDOORSWITCH + 1: return SPACEDOORSWITCH;
    case SPACELIGHTSWITCH: case SPACELIGHTSWITCH + 1: return SPACELIGHTSWITCH;
    case FRANKENSTINESWITCH: case FRANKENSTINESWITCH + 1: return FRANKENSTINESWITCH;
    case SLOTDOOR: case SLOTDOOR + 1: return SLOTDOOR;
    case ACCESSSWITCH: return ACCESSSWITCH;
    case ACCESSSWITCH2: return ACCESSSWITCH2;
    case MULTISWITCH: case MULTISWITCH + 1: case MULTISWITCH + 2: case MULTISWITCH + 3: return MULTISWITCH;
    case NUKEBUTTON: case NUKEBUTTON + 1: return NUKEBUTTON;
    }
    return -1;
}

/* switchtype 0 = wall, 1 = sprite; returns 1 if a switch was operated */
int checkhitswitch(int w, int switchtype)
{
    int16_t *picp, lotag, hitag, pal;
    int32_t sx, sy;
    if (switchtype) {
        picp = &sprite[w].picnum; lotag = sprite[w].lotag; hitag = sprite[w].hitag; pal = sprite[w].pal;
        sx = sprite[w].x; sy = sprite[w].y;
    } else {
        picp = &wall[w].picnum; lotag = wallx[w].lotag; hitag = wallx[w].hitag; pal = wall[w].pal;
        sx = wall[w].x; sy = wall[w].y;
    }
    (void)hitag; (void)sx; (void)sy;
    int base = switch_base(*picp);
    if (base < 0) return 0;
    if (base == NUKEBUTTON) {
        if (*picp == NUKEBUTTON) {
            *picp = NUKEBUTTON + 1;
            pl.level_done = 1;
            pl.timebeforeexit = 30 * 3;
            globalsound(SWITCH_ON);
        }
        return 1;
    }
    if (lotag == 0 && base != LIGHTSWITCH && base != LIGHTSWITCH2) { /* decorative */ }
    if (base == ACCESSSWITCH || base == ACCESSSWITCH2) {
        int need = (pal == 0) ? 1 : (pal == 21) ? 2 : (pal == 23) ? 4 : 0;
        if (need && !(pl.keys & need)) {
            player_quote(need == 1 ? 70 : need == 2 ? 71 : 72);
            return 1;
        }
    }
    int puzzle = (base == DIPSWITCH || base == TECHSWITCH || base == ALIENSWITCH);
    int correct = 1, count = 0;
    int original_pic = *picp;
    /* DIP/TECH/ALIEN puzzle switches move individually; all the others
     * sharing a tag change together.  Count the other puzzle switches in
     * their required hitag positions, just as SECTOR.C does. */
    for (int i = 0; i < MAXSPRITES; i++) {
        if (sprite[i].statnum != STAT_DEFAULT || sprite[i].lotag != lotag || !lotag) continue;
        int b = switch_base(sprite[i].picnum);
        if (b < 0) continue;
        if (b == DIPSWITCH || b == TECHSWITCH || b == ALIENSWITCH) {
            count++;
            if (switchtype && i == w) sprite[i].picnum = (int16_t)(sprite[i].picnum == b ? b + 1 : b);
            else if ((sprite[i].picnum - b) == sprite[i].hitag) correct++;
        } else if (b == MULTISWITCH) {
            sprite[i].picnum = (int16_t)(MULTISWITCH + ((sprite[i].picnum - MULTISWITCH + 1) & 3));
        } else if (b != ACCESSSWITCH && b != ACCESSSWITCH2 && b != NUKEBUTTON) {
            sprite[i].picnum = (int16_t)(sprite[i].picnum == b ? b + 1 : b);
        }
    }
    for (int i = 0; i < numwalls; i++) {
        int b = switch_base(wall[i].picnum);
        if (b < 0 || wallx[i].lotag != lotag || !lotag) continue;
        if (b == DIPSWITCH || b == TECHSWITCH || b == ALIENSWITCH) {
            count++;
            if (!switchtype && i == w) wall[i].picnum = (int16_t)(wall[i].picnum == b ? b + 1 : b);
            else if ((wall[i].picnum - b) == wallx[i].hitag) correct++;
        } else if (b == MULTISWITCH) {
            wall[i].picnum = (int16_t)(MULTISWITCH + ((wall[i].picnum - MULTISWITCH + 1) & 3));
        } else if (b != ACCESSSWITCH && b != ACCESSSWITCH2 && b != NUKEBUTTON) {
            wall[i].picnum = (int16_t)(wall[i].picnum == b ? b + 1 : b);
        }
    }
    if (!lotag && base != ACCESSSWITCH && base != ACCESSSWITCH2)
        *picp = (int16_t)(base == MULTISWITCH ? MULTISWITCH + ((*picp - MULTISWITCH + 1) & 3) : (*picp == base ? base + 1 : base));
    if (switchtype) spritesound(SWITCH_ON, w); else globalsound(SWITCH_ON);
    if (puzzle && count != correct) return 1;
    if (base == MULTISWITCH) lotag += original_pic - MULTISWITCH;
    if (lotag) {
        for (int i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
            if (sprite[i].hitag == lotag && (sprite[i].lotag == 24 || sprite[i].lotag == 34 || sprite[i].lotag == 25))
                T5(i) = !T5(i);
        operateactivators(lotag, 0);
        operatemasterswitches(lotag);
    }
    return 1;
}

/* ---------------------------------------------------------- wall hits */
void checkhitwall(int spr, int dawallnum, int32_t x, int32_t y, int32_t z, int atwith)
{
    walltype *wal = &wall[dawallnum];
    (void)spr; (void)x; (void)y; (void)z;
    if (wal->overpicnum == MIRROR) {
        wal->overpicnum = MIRRORBROKE;
        lotsofglass(-1, dawallnum, 70);
        wal->cstat &= ~16;
        spritesound(GLASS_HEAVYBREAK, -1);
        return;
    }
    if (((wal->cstat & 16) || wal->overpicnum == BIGFORCE) && wal->nextsector >= 0) {
        if (sector[wal->nextsector].floorz > z && sector[wal->nextsector].floorz - sector[wal->nextsector].ceilingz) {
            switch (wal->overpicnum) {
            case GLASS:
            case STAINGLASS1:
                wal->overpicnum = GLASS2;
                lotsofglass(-1, dawallnum, 10);
                wal->cstat = 0;
                if (wallx[dawallnum].nextwall >= 0) wall[wallx[dawallnum].nextwall].cstat = 0;
                spritesound(GLASS_BREAKING, -1);
                return;
            case FANSPRITE:
                wal->overpicnum = FANSPRITEBROKE;
                wal->cstat &= 65535 - 65;
                if (wallx[dawallnum].nextwall >= 0) {
                    wall[wallx[dawallnum].nextwall].overpicnum = FANSPRITEBROKE;
                    wall[wallx[dawallnum].nextwall].cstat &= 65535 - 65;
                }
                spritesound(VENT_BUST, -1);
                spritesound(GLASS_BREAKING, -1);
                return;
            }
        }
    }
    switch (wal->picnum) {
    case COLAMACHINE: case VENDMACHINE:
        wal->picnum++;
        spritesound(VENT_BUST, -1);
        return;
    case OJ: case FEMPIC2: case FEMPIC3: case SCREENBREAK6: case SCREENBREAK7: case SCREENBREAK8:
    case SCREENBREAK1: case SCREENBREAK2: case SCREENBREAK3: case SCREENBREAK4: case SCREENBREAK5:
    case SCREENBREAK9: case SCREENBREAK10: case SCREENBREAK11: case SCREENBREAK12: case SCREENBREAK13:
    case SCREENBREAK14: case SCREENBREAK15: case SCREENBREAK16: case SCREENBREAK17: case SCREENBREAK18:
    case SCREENBREAK19: case BORNTOBEWILDSCREEN:
        lotsofglass(-1, dawallnum, 30);
        wal->picnum = W_SCREENBREAK + (krand() % 3);
        spritesound(GLASS_HEAVYBREAK, -1);
        return;
    case W_TECHWALL5: case W_TECHWALL6: case W_TECHWALL7: case W_TECHWALL8: case W_TECHWALL9:
        wal->picnum = W_TECHWALL5 == wal->picnum ? W_HITTECHWALL4 : W_HITTECHWALL10;
        return;
    case W_MILKSHELF:
        wal->picnum = W_MILKSHELFBROKE;
        lotsofglass(-1, dawallnum, 10);
        spritesound(GLASS_HEAVYBREAK, -1);
        return;
    case ATM:
        wal->picnum = ATMBROKE;
        spritesound(GLASS_HEAVYBREAK, -1);
        return;
    case WALLLIGHT1: case WALLLIGHT2: case WALLLIGHT3: case WALLLIGHT4:
    case TECHLIGHT2: case TECHLIGHT4:
        if (rnd(128)) spritesound(GLASS_HEAVYBREAK, -1); else spritesound(GLASS_BREAKING, -1);
        lotsofglass(-1, dawallnum, 30);
        wal->picnum++;
        /* lights in that sector go dark (SE 3/4 by wall lotag) */
        for (int i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
            if (sprite[i].hitag == wallx[dawallnum].lotag && wallx[dawallnum].lotag && sprite[i].lotag == 3) {
                T3(i) = 1; T4(i) = 1;
            }
        return;
    }
    (void)atwith;
}

/* A tagged sector between Duke and a wall-mounted switch can truncate the
 * standard neartag ray.  Retry just for switch sprites, ignoring sector tags
 * and using a small angular fan.  The normal straight-ahead interaction
 * always gets first refusal; line of sight prevents reaching through walls. */
static int nearby_switch(void)
{
    static const int16_t offsets[] = { 0, -32, 32, -64, 64, -96, 96, -128, 128 };
    for (unsigned a = 0; a < sizeof(offsets) / sizeof(offsets[0]); a++) {
        for (int h = 0; h <= 16; h += 8) {
            int16_t ns, nw, sp; int32_t dist;
            neartag(pl.posx, pl.posy, pl.posz + (h << 8), pl.cursectnum,
                    (pl.ang + offsets[a]) & 2047, &ns, &nw, &sp, &dist, 1280, 5);
            if (sp < 0 || dist < 0 || dist > 1280 || switch_base(sprite[sp].picnum) < 0) continue;
            if (!cansee(pl.posx, pl.posy, pl.posz + (h << 8), pl.cursectnum,
                        sprite[sp].x, sprite[sp].y, pl.posz + (h << 8), sprite[sp].sectnum)) continue;
            return checkhitswitch(sp, 1);
        }
    }
    return 0;
}

/* ------------------------------------------------------- player use */
void checksectors(void)
{
    int16_t neartagsector, neartagwall, neartagsprite;
    int32_t neartaghitdist;
    sectortype *ps = &sector[pl.cursectnum];

    switch (ps->lotag) {
    case 32767:
        ps->lotag = 0;
        player_quote(9);
        pl.secret_rooms++;
        globalsound(SECRETLEVELSND);
        return;
    case -1:
        ps->lotag = 0;
        pl.level_done = 1;
        pl.timebeforeexit = 1;
        return;
    case -2:
        ps->lotag = 0;
        pl.level_done = 1;
        pl.timebeforeexit = 26 * 8;
        return;
    default:
        if (ps->lotag >= 10000 && ps->lotag < 16383) {
            if (pl.cursectnum >= 0) spritesound(ps->lotag - 10000, -1);
            ps->lotag = 0;
        }
        break;
    }
    if (!(sync1.bits & SB_USE)) return;
    if (pl.dead_flag) return;

    neartag(pl.posx, pl.posy, pl.posz, pl.cursectnum, pl.ang, &neartagsector, &neartagwall,
            &neartagsprite, &neartaghitdist, 1280, 1);
    if (neartagsector == -1 && neartagwall == -1 && neartagsprite == -1)
        neartag(pl.posx, pl.posy, pl.posz + (8 << 8), pl.cursectnum, pl.ang, &neartagsector, &neartagwall,
                &neartagsprite, &neartaghitdist, 1280, 1);
    if (neartagsector == -1 && neartagwall == -1 && neartagsprite == -1)
        neartag(pl.posx, pl.posy, pl.posz + (16 << 8), pl.cursectnum, pl.ang, &neartagsector, &neartagwall,
                &neartagsprite, &neartaghitdist, 1280, 3);
    if (neartagsprite >= 0) {
        spritetype *s = &sprite[neartagsprite];
        if (checkhitswitch(neartagsprite, 1)) return;
        switch (s->picnum) {
        case TOILET: case STALL:
            if (!sound_playing(FLUSH_TOILET)) {
                spritesound(FLUSH_TOILET, neartagsprite);
                if (pl.health < CON_MAXPLAYERHEALTH && pl.health > 0) { /* Duke "relief" */ }
                if (rnd(64)) player_talk(DUKE_URINATE);
            }
            return;
        case WATERFOUNTAIN:
            if (pl.health < CON_MAXWATERFOUNTAINHEALTH && pl.health > 0) {
                pl.health++;
                hud_dirty = 2;
                if (!sound_playing(DUKE_DRINKING)) spritesound(DUKE_DRINKING, neartagsprite);
            }
            return;
        case PLUG:
            spritesound(SHORT_CIRCUIT, -1);
            player_hurt(2 + (krand() & 3), -1);
            return;
        case VIEWSCREEN: case VIEWSCREEN2:
            for (int i = headspritestat[STAT_ACTOR]; i >= 0; i = nextspritestat[i])
                if (sprite[i].picnum == CAMERA1 && sprite[i].yvel == 0 && s->hitag == sprite[i].lotag) {
                    camsprite = (int16_t)i;
                    return;
                }
            camsprite = -1;
            return;
        }
    }
    if ((neartagwall < 0 || switch_base(wall[neartagwall].picnum) < 0) &&
        (neartagsector < 0 || !isanearoperator(sector[neartagsector].lotag)) &&
        nearby_switch()) return;
    if (neartagwall == -1 && neartagsector == -1 && neartagsprite == -1) {
        /* SECTOR.C: nothing tagged in range - only give up (Duke searches the
         * blank wall) when facing a wall closer than 512 (hits() < 512);
         * otherwise fall through so the sector we stand in (lifts, warp
         * elevators: under-operators) can be operated. */
        int16_t hs, hw, hsp; int32_t hx, hy, hz;
        hitscan(pl.posx, pl.posy, pl.posz, pl.cursectnum, sintable[(pl.ang + 512) & 2047],
                sintable[pl.ang & 2047], 0, &hs, &hw, &hsp, &hx, &hy, &hz, CLIPMASK1);
        if (hs >= 0 && klabs(hx - pl.posx) + klabs(hy - pl.posy) < 512) return;
    }
    if (neartagwall >= 0) {
        if (wallx[neartagwall].lotag > 0 && switch_base(wall[neartagwall].picnum) >= 0) {
            checkhitswitch(neartagwall, 0);
            return;
        }
        if (wall[neartagwall].nextsector >= 0 && wall[neartagwall].nextsector < numsectors) {
            int ns = wall[neartagwall].nextsector;
            if (isanearoperator(sector[ns].lotag) && !(sector[ns].lotag & 16384)) {
                activatebysector(ns, pl.i);
                return;
            }
        }
    }
    if (neartagsector >= 0 && (sector[neartagsector].lotag & 16384) == 0 &&
        isanearoperator(sector[neartagsector].lotag)) {
        if ((sector[neartagsector].lotag & 0xff) == 23 || (sector[neartagsector].lotag & 0xff) == 25 ||
            (sector[neartagsector].lotag & 0xff) == 9) {
            activatebysector(neartagsector, pl.i);
            return;
        }
        operatesectors(neartagsector, pl.i);
        return;
    }
    if ((sector[pl.cursectnum].lotag & 16384) == 0 && isanunderoperator(sector[pl.cursectnum].lotag)) {
        activatebysector(pl.cursectnum, pl.i);
        return;
    }
    if (neartagsector >= 0 && (sector[neartagsector].lotag & 16384)) player_quote(41);
}

/* ------------------------------------------------------- effectors */
void sector_init_level(void)
{
    animatecnt = 0;
    msnum = 0;
    earthquaketime = 0;
    camsprite = -1;
    for (int i = 0; i < numsectors; i++) {
        sector[i].extra = 256;                  /* PREMAP.C prelevel(): default speed */
        if ((sector[i].lotag & 0xff) == 20 || (sector[i].lotag & 0xff) == 22)
            if (sector[i].floorz > sector[i].ceilingz) sector[i].lotag |= 32768;
    }
    /* PREMAP.C: forcefields must block both movement and hitscan; their
     * map cstat often omits the hitscan bit.  Dark ones start switched off. */
    for (int w = 0; w < numwalls; w++) {
        int pic = wall[w].overpicnum;
        if (pic >= W_FORCEFIELD && pic <= W_FORCEFIELD + 2) {
            if (wall[w].shade > 31) wall[w].cstat = 0;
            else wall[w].cstat |= 85 | 256;
        }
    }
    /* PREMAP.C: GPSPEED sprites (stat 0) set their sector's door/lift speed,
     * before any sector effector copies sector[].extra into its yvel (SP). */
    for (int i = 0; i < MAXSPRITES; i++)
        if (sprite[i].statnum == 0 && sprite[i].picnum == GPSPEED) {
            sector[sprite[i].sectnum].extra = sprite[i].lotag;
            deletesprite((int16_t)i);
        }
}

/* ------------------------------------------- SE 17 warp elevators (ST 15)
 * SECTOR.C activatewarpelevators(): start (d = -1 up, +1 down) or stop (0)
 * every SE 17 car sharing this hitag.  Returns 1 if no car in that
 * direction exists.  SP (speed) is the effector's yvel = sector extra. */
static int activatewarpelevators(int s, int d)
{
    int sn = sprite[s].sectnum, i;
    for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
        if (sprite[i].lotag == 17 && sprite[i].hitag == sprite[s].hitag)
            if (klabs(sector[sn].floorz - act[s].t[2]) > sprite[i].yvel ||
                sector[sprite[i].sectnum].hitag == sector[sn].hitag - d)
                break;
    if (i < 0) return 1;
    spritesound(d == 0 ? ELEVATOR_OFF : ELEVATOR_ON, s);
    for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
        if (sprite[i].lotag == 17 && sprite[i].hitag == sprite[s].hitag) {
            act[i].t[0] = d;
            act[i].t[1] = d;        /* make all check warp */
        }
    return 0;
}

/* SECTOR.C allignwarpelevators() (called by enterlevel after prelevel):
 * a car whose SE 17 has shade > 16 is the one the player starts at; every
 * other car with the same hitag is parked at its absolute height (out of
 * sight in the shaft) while keeping its own stopping height (T3). */
void align_warp_elevators(void)
{
    for (int i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i]) {
        if (sprite[i].lotag != 17 || sprite[i].shade <= 16) continue;
        for (int j = headspritestat[STAT_EFFECTOR]; j >= 0; j = nextspritestat[j])
            if (sprite[j].lotag == 17 && j != i && sprite[j].hitag == sprite[i].hitag) {
                sector[sprite[j].sectnum].floorz = sector[sprite[i].sectnum].floorz;
                sector[sprite[j].sectnum].ceilingz = sector[sprite[i].sectnum].ceilingz;
            }
    }
}

void effector_spawn(int i)
{
    spritetype *sp = &sprite[i];
    int sect = sp->sectnum, s, startwall = sector[sect].wallptr, endwall = startwall + sector[sect].wallnum;
    memset(&act[i], 0, sizeof(actor_t));
    sp->yvel = sector[sect].extra;
    sp->cstat |= 32768;
    sp->xrepeat = sp->yrepeat = 0;
    switch (sp->lotag) {
    case 7: case 23:      /* transporters: owner = partner with same hitag */
        sp->owner = (int16_t)i;
        for (int j = 0; j < MAXSPRITES; j++)
            if (j != i && sprite[j].statnum < MAXSTATUS && sprite[j].picnum == SECTOREFFECTOR &&
                (sprite[j].lotag == 7 || sprite[j].lotag == 23) && sprite[j].hitag == sp->hitag) {
                sp->owner = (int16_t)j; break;
            }
        T5(i) = sector[sect].floorz == sp->z;
        sp->cstat = 32768;
        changespritestat((int16_t)i, STAT_TRANSPORT);
        return;
    case 24: /* GAME.C doubles SE 24's map speed; SE 34 is not doubled. */
        sp->yvel = (int16_t)(sp->yvel << 1);
        break;
    case 1: sp->owner = -1; T1(i) = 1; break;
    case 12: T2(i) = sector[sect].floorshade; T3(i) = sector[sect].ceilingshade; break;
    case 13:
        T1(i) = sector[sect].ceilingz;
        T2(i) = sector[sect].floorz;
        sp->owner = (klabs(T1(i) - sp->z) < klabs(T2(i) - sp->z)) ? 1 : 0;
        if (sp->ang == 512) {
            if (sp->owner) sector[sect].ceilingz = sp->z; else sector[sect].floorz = sp->z;
        } else
            sector[sect].ceilingz = sector[sect].floorz = sp->z;
        if (sector[sect].ceilingstat & 1) {
            sector[sect].ceilingstat ^= 1;
            T4(i) = 1;
            if (!sp->owner && sp->ang == 512) { sector[sect].ceilingstat ^= 1; T4(i) = 0; }
            sector[sect].ceilingshade = sector[sect].floorshade;
            if (sp->ang == 512)
                for (s = startwall; s < endwall; s++) {
                    int x = wall[s].nextsector;
                    if (x >= 0 && !(sector[x].ceilingstat & 1)) {
                        sector[sect].ceilingpicnum = sector[x].ceilingpicnum;
                        sector[sect].ceilingshade = sector[x].ceilingshade;
                        break;
                    }
                }
        }
        break;
    case 3:
        T4(i) = sector[sect].floorshade;
        sector[sect].floorshade = sp->shade;
        sector[sect].ceilingshade = sp->shade;
        sp->owner = (int16_t)((sector[sect].ceilingpal << 8) | sector[sect].floorpal);
        for (s = startwall; s < endwall; s++)
            if (!(wallx[s].hitag & 1)) wall[s].shade = sp->shade;
        break;
    case 4:
        T3(i) = sector[sect].floorshade;
        sp->owner = (int16_t)((sector[sect].ceilingpal << 8) | sector[sect].floorpal);
        for (s = startwall; s < endwall; s++) if (wall[s].shade > T4(i)) T4(i) = wall[s].shade;
        break;
    case 9:
        if (sector[sect].lotag && klabs(sector[sect].ceilingz - sp->z) > 1024) sector[sect].lotag |= 32768;
        /* fall through */
    case 8:
        T1(i) = sector[sect].floorshade;
        T2(i) = sector[sect].ceilingshade;
        for (s = startwall; s < endwall; s++) if (wall[s].shade > T3(i)) T3(i) = wall[s].shade;
        T4(i) = 1;
        break;
    case 17: {            /* warp elevator car (GAME.C spawn) */
        int j;
        T3(i) = sector[sect].floorz;                    /* stopping height */
        /* T4/T5 are 16-bit: keep the out-of-sight heights as z >> 8 */
        j = nextsectorneighborz((int16_t)sect, sector[sect].floorz, -1, -1);
        T4(i) = (int16_t)((j >= 0 ? sector[j].ceilingz : sector[sect].ceilingz) >> 8);
        j = nextsectorneighborz((int16_t)sect, sector[sect].ceilingz, 1, 1);
        T5(i) = (int16_t)((j >= 0 ? sector[j].floorz : sector[sect].floorz) >> 8);
        break;
    }
    case 31:
        T2(i) = sector[sect].floorz;
        if (sp->ang != 1536) sector[sect].floorz = sp->z;
        break;
    case 32:
        T2(i) = sector[sect].ceilingz;
        T3(i) = sp->hitag;
        if (sp->ang != 1536) sector[sect].ceilingz = sp->z;
        break;
    case 25:
        T4(i) = sector[sect].ceilingz;
        T5(i) = 1;
        sector[sect].ceilingz = sp->z;
        break;
    case 11:
        T4(i) = (sp->ang > 1024) ? 2 : -2;
        /* fall through */
    case 0: case 2: case 15: case 16: case 26: case 30:
        if (sp->lotag == 0) {
            int j;
            for (j = 0; j < MAXSPRITES; j++)
                if (sprite[j].statnum < MAXSTATUS && sprite[j].picnum == SECTOREFFECTOR &&
                    sprite[j].lotag == 1 && sprite[j].hitag == sp->hitag) {
                    if (sp->ang == 512) { sp->x = sprite[j].x; sp->y = sprite[j].y; }
                    break;
                }
            sp->owner = (int16_t)(j < MAXSPRITES ? j : i);
        }
        record_ms(i);
        if (sp->lotag == 30) {
            /* Non-rotating subway: remember the surrounding locator sector.
             * The sector hitag becomes the effector index, as in Duke. */
            sp->extra = (sector[sect].hitag == -1) ? 0 : 1;
            sector[sect].hitag = (int16_t)i;
            T1(i) = -1;
            for (s = startwall; s < endwall; s++) {
                int ns = wall[s].nextsector;
                if (ns >= 0 && sector[ns].hitag == 0 && sector[ns].lotag < 3) {
                    T1(i) = ns; break;
                }
            }
            sp->owner = -1;
        }
        if (sp->lotag == 2) T6(i) = (int16_t)(sector[sect].floorheinum);
        break;
    }
    changespritestat((int16_t)i, STAT_EFFECTOR);
}

/* ACTORS.C (SEENINE detonation, exploding barrels): start the SE 13
 * breakaways of this hitag - "if (temp_data[2] == 0) temp_data[2] = 1".
 * T4 is left alone: it is 1 only when spawning took a parallax ceiling away
 * (the explosion then gives it back).  Forcing it to 1 here used to strip
 * the sky off breakaways that kept theirs - E1L1's vent housing then drew
 * its walls up into the sky ("a piece of the vent in the air"). */
void triggerse13(int hitag)
{
    for (int i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nextspritestat[i])
        if (sprite[i].lotag == 13 && sprite[i].hitag == hitag && T3(i) == 0) T3(i) = 1;
}

static inline int32_t sgn32(int32_t v) { return (v > 0) - (v < 0); }

/* SE 31: move a sector floor and whatever stands on it (player on the ground,
 * resting sprites) - ACTORS.C */
static void se31_shift(int i, int32_t l)
{
    int sect = sprite[i].sectnum;
    sector[sect].floorz += l;
    if (pl.cursectnum == sect && pl.on_ground) pl.posz += l;
    for (int j = headspritesect[sect]; j >= 0; j = nextspritesect[j])
        if (sprite[j].zvel == 0 && sprite[j].statnum != STAT_EFFECTOR && sprite[j].statnum != STAT_PROJECTILE && j != pl.i)
            sprite[j].z += l;
}

void moveeffectors(void)
{
    int i, nexti, j, k, x, q, l;
    for (i = headspritestat[STAT_EFFECTOR]; i >= 0; i = nexti) {
        nexti = nextspritestat[i];
        spritetype *s = &sprite[i];
        sectortype *sc = &sector[s->sectnum];
        switch (s->lotag) {
        case 0: {          /* rotating sector around its SE 1 pivot */
            j = s->owner;
            if (j < 0 || j >= MAXSPRITES) break;
            q = sc->extra >> 3;
            if (T1(j) == 0) break;
            l = (sprite[j].ang > 1024) ? -1 : 1;
            if (T4(i) == 0) T4(i) = ksqrt((uint32_t)((s->x - sprite[j].x) * (s->x - sprite[j].x) +
                                                    (s->y - sprite[j].y) * (s->y - sprite[j].y)));
            s->x = sprite[j].x; s->y = sprite[j].y;
            s->ang += (int16_t)(l * q);
            T3(i) += l * q;
            if (sc->floorstat & 64 || 1) {
                if (pl.cursectnum == s->sectnum && pl.on_ground) {
                    int32_t m, xx;
                    pl.ang = (int16_t)((pl.ang + l * q) & 2047);
                    rotatepoint(sprite[j].x, sprite[j].y, pl.posx, pl.posy, (int16_t)(q * l), &m, &xx);
                    pl.posx = m; pl.posy = xx;
                }
                for (int p = headspritesect[s->sectnum]; p >= 0; p = nextspritesect[p])
                    if (sprite[p].statnum != STAT_EFFECTOR && sprite[p].statnum != STAT_PROJECTILE && p != pl.i) {
                        int32_t m, xx;
                        sprite[p].ang = (int16_t)((sprite[p].ang + l * q) & 2047);
                        rotatepoint(sprite[j].x, sprite[j].y, sprite[p].x, sprite[p].y, (int16_t)(q * l), &m, &xx);
                        sprite[p].x = m; sprite[p].y = xx;
                    }
            }
            s->xvel = 0;
            ms(i);
            break;
        }
        case 2:            /* earthquake */
            if (T5(i) > 0 && T1(i) == 0) {
                if (T5(i) < 2) { T1(i) = 1; earthquaketime = 48; spritesound(EARTHQUAKE, -1); }
                T5(i)--;
            }
            if (T1(i) > 0 && T1(i) < 96) {
                T1(i)++;
                sc->floorheinum = (int16_t)((krand() & 16) - 8 + T6(i));
                if (T1(i) >= 96) sc->floorheinum = T6(i);
            }
            break;
        case 3:            /* flickering lights */
            if (T5(i) == 0 && !(krand() & 7)) {
                x = (krand() & 1) ? s->shade : T4(i);
                sc->floorshade = sc->ceilingshade = (int8_t)x;
                int sw = sc->wallptr;
                for (k = 0; k < sc->wallnum; k++) if (!(wallx[sw + k].hitag & 1)) wall[sw + k].shade = (int8_t)x;
            }
            break;
        case 4:            /* flashing lights (on timer) */
            if ((krand() & 15) == 0) {
                x = ((gametic >> 2) & 1) ? T3(i) : T4(i);
                if (T5(i)) x = T4(i);
                sc->floorshade = sc->ceilingshade = (int8_t)x;
                int sw = sc->wallptr;
                for (k = 0; k < sc->wallnum; k++)
                    if (wallx[sw + k].hitag != 1) wall[sw + k].shade = (int8_t)x;
            }
            break;
        case 8: case 9: {  /* door lights follow the door's opening */
            int32_t span = klabs(sc->floorz - s->z);
            int32_t open = klabs(sc->ceilingz - s->z);
            if (span == 0) span = 1;
            x = T1(i) + (int32_t)(((int64_t)(T3(i) - T1(i)) * (span - min(open, span))) / span);
            (void)x;
            break;
        }
        case 10: {         /* ACTORS.C: timed doors close after Duke leaves */
            int lt = sc->lotag & 0xff;
            if (lt == 27 || (sc->floorz > sc->ceilingz && lt != 23) || sc->lotag == (int16_t)32791) {
                if (lt != 27 && lt != 30 && lt != 31 && lt != 0 && pl.cursectnum == s->sectnum) break;
                if (T1(i) > s->hitag) {
                    if ((lt == 20 || lt == 21 || lt == 22 || lt == 26) &&
                        getanimationgoal(&sc->ceilingz) >= 0) break;
                    activatebysector(s->sectnum, i);
                    T1(i) = 0;
                } else T1(i)++;
            } else T1(i) = 0;
            break;
        }
        case 11:           /* swinging door */
            if (T6(i) > 0) { T6(i)--; break; }
            if (T5(i)) {
                k = (s->yvel >> 3) * T4(i);
                T3(i) += k;
                T5(i) += k;
                s->xvel = 0;
                ms(i);
                setsprite((int16_t)i, s->x, s->y, s->z);
                if (T5(i) <= -511 || T5(i) >= 512) {
                    T5(i) = 0;
                    T3(i) &= 0xffffff00;
                    ms(i);
                    setsprite((int16_t)i, s->x, s->y, s->z);
                }
            }
            break;
        case 12:           /* light switch */
            if (T1(i) == 2 || T4(i) == 1) {       /* lights off */
                sc->floorshade = sc->ceilingshade = (int8_t)min(30, T2(i) + 16);
                int sw = sc->wallptr;
                for (k = 0; k < sc->wallnum; k++) if (wallx[sw + k].hitag != 1) wall[sw + k].shade = (int8_t)min(30, T2(i) + 16);
                T4(i) = 0;
                if (T1(i) == 2) T1(i) = 3;
            }
            if (T1(i) == 1) {                     /* lights on */
                sc->floorshade = (int8_t)T2(i);
                sc->ceilingshade = (int8_t)T3(i);
                int sw = sc->wallptr;
                for (k = 0; k < sc->wallnum; k++) if (wallx[sw + k].hitag != 1) wall[sw + k].shade = (int8_t)T2(i);
                T1(i) = 0;
            }
            break;
        case 13:           /* C-9 breakaway (cracked walls / blown ceilings) */
            if (T3(i)) {
                j = (s->yvel << 5) | 1;
                if (s->ang == 512) {
                    if (s->owner) {
                        if (klabs(T1(i) - sc->ceilingz) >= j) sc->ceilingz += ksgn(T1(i) - sc->ceilingz) * j;
                        else sc->ceilingz = T1(i);
                    } else {
                        if (klabs(T2(i) - sc->floorz) >= j) sc->floorz += ksgn(T2(i) - sc->floorz) * j;
                        else sc->floorz = T2(i);
                    }
                } else {
                    if (klabs(T2(i) - sc->floorz) >= j) sc->floorz += ksgn(T2(i) - sc->floorz) * j;
                    else sc->floorz = T2(i);
                    sc->ceilingz = T1(i);
                }
                if (T4(i) == 1) {
                    T4(i)++;
                    sc->ceilingstat ^= 1;
                    if (s->ang == 512) {
                        int sw = sc->wallptr;
                        for (k = 0; k < sc->wallnum; k++) wall[sw + k].shade = s->shade;
                        sc->floorshade = s->shade;
                    }
                }
                T3(i)++;
                if (T3(i) == 4 && s->ang != 512) {
                    for (x = 0; x < 6; x++)
                        spawn_sprite(s->sectnum, s->x, s->y, s->z - (8 << 8), SCRAP1 + (krand() & 15), -8, 48, 48,
                                     krand() & 2047, (krand() & 63) + 64, -512 - (krand() & 2047), i, STAT_MISC);
                }
                if (T3(i) > 256) deletesprite((int16_t)i);
            }
            break;
        case 15:           /* sliding door */
            if (T5(i)) {
                s->xvel = 16;
                if (T5(i) == 1) {
                    if (T4(i) >= (s->yvel >> 3)) { T5(i) = 0; callsound(s->sectnum, i); break; }
                    T4(i)++;
                } else {
                    if (T4(i) < 1) { T5(i) = 0; callsound(s->sectnum, i); break; }
                    T4(i)--;
                }
                ms(i);
                setsprite((int16_t)i, s->x, s->y, s->z);
            }
            break;
        case 24: case 34:  /* conveyor floors (34 is the water-current variant) */
            if (T5(i) == 0) {
                x = (s->yvel * sintable[(s->ang + 512) & 2047]) >> 18;
                l = (s->yvel * sintable[s->ang & 2047]) >> 18;
                sc->floorxpanning += (uint8_t)(s->yvel >> 7);
                /* Original currents feed the next player input as friction;
                 * use velocity rather than teleporting through solid walls. */
                if (pl.cursectnum == s->sectnum && pl.on_ground &&
                    klabs(pl.posz - pl.truefz) < PHEIGHT + (9 << 8)) {
                    pl.posxv += x * 2048; pl.posyv += l * 2048;
                }
                int nextj;
                for (j = headspritesect[s->sectnum]; j >= 0; j = nextj) {
                    nextj = nextspritesect[j];
                    spritetype *p = &sprite[j];
                    if (j == pl.i || p->zvel < 0 ||
                        (p->statnum != STAT_ITEM && p->statnum != STAT_DEAD &&
                         p->statnum != STAT_ACTOR && p->statnum != STAT_STANDABLE)) continue;
                    if (p->statnum == STAT_STANDABLE && (p->picnum == TOUCHPLATE || p->picnum == MASTERSWITCH)) continue;
                    if (p->z <= getflorzofslope(s->sectnum, p->x, p->y) - (16 << 8)) continue;
                    p->x += x >> 2; p->y += l >> 2;
                    setsprite((int16_t)j, p->x, p->y, p->z);
                }
            }
            break;
        case 30: {         /* non-rotating subway, ACTORS.C */
            if (s->owner < 0) {
                T4(i) = !T4(i);
                for (j = headspritestat[STAT_LOCATOR]; j >= 0; j = nextspritestat[j])
                    if (sprite[j].picnum == LOCATORS && sprite[j].lotag == T4(i) && sprite[j].sectnum == T1(i)) break;
                if (j < 0) { T4(i) = !T4(i); break; }
                s->owner = (int16_t)j;
            } else {
                int32_t dx = klabs(sprite[s->owner].x - s->x), dy = klabs(sprite[s->owner].y - s->y);
                int32_t dist = max(dx, dy) + (min(dx, dy) >> 1);
                if (T5(i) == 1) {
                    if (dist < 1920) T5(i) = 2;
                    else {
                        if (s->xvel == 0) operateactivators(s->hitag + !T4(i), -1);
                        if (s->xvel < 256) s->xvel += 16;
                    }
                }
                if (T5(i) == 2) {
                    if (dist <= 128) s->xvel = 0;
                    if (s->xvel > 0) s->xvel -= 16;
                    else {
                        s->xvel = 0;
                        operateactivators(s->hitag + T4(i), -1);
                        s->owner = -1; s->ang = (s->ang + 1024) & 2047; T5(i) = 0;
                    }
                }
            }
            if (s->xvel) {
                int32_t dx = mulscale14(s->xvel, sintable[(s->ang + 512) & 2047]);
                int32_t dy = mulscale14(s->xvel, sintable[s->ang & 2047]);
                if (pl.cursectnum == s->sectnum) {
                    pl.posx += dx; pl.posy += dy; pl.bobposx += dx; pl.bobposy += dy;
                }
                for (j = headspritesect[s->sectnum]; j >= 0; j = nextspritesect[j])
                    if (j != pl.i && sprite[j].picnum != SECTOREFFECTOR && sprite[j].picnum != LOCATORS) {
                        sprite[j].x += dx; sprite[j].y += dy;
                    }
                ms(i);
                setsprite((int16_t)i, s->x, s->y, s->z);
            }
            break;
        }
        case 29:           /* sine-wave floor (Toxic Dump water and machinery) */
            s->hitag = (int16_t)((s->hitag + 64) & 2047);
            sc->floorz = s->z + mulscale12(s->yvel, sintable[s->hitag & 2047]);
            break;
        case 17: {         /* warp elevator car - ACTORS.C moveeffectors */
            q = T1(i) * (s->yvel << 2);
            if (q) {
                sc->ceilingz += q;
                sc->floorz += q;
                for (j = headspritesect[s->sectnum]; j >= 0; j = nextspritesect[j])
                    if (sprite[j].statnum != STAT_EFFECTOR && j != pl.i) sprite[j].z += q;
                if (pl.cursectnum == s->sectnum) {
                    pl.posz += q; pl.oposz = pl.posz; pl.truefz += q; pl.truecz += q;
                }
            }
            if (!T1(i)) break;                              /* not in motion */
            if (klabs(sc->floorz - T3(i)) <= s->yvel) {     /* arrived: stop every car */
                activatewarpelevators(i, 0);
                break;
            }
            /* while the opening is still visible we can't teleport yet */
            if (T1(i) == -1) { if (sc->floorz > ((int32_t)T4(i) << 8)) break; }
            else if (sc->ceilingz < ((int32_t)T5(i) << 8)) break;
            if (T2(i) == 0) break;
            T2(i) = 0;
            for (j = headspritestat[STAT_EFFECTOR]; j >= 0; j = nextspritestat[j])
                if (i != j && sprite[j].lotag == 17 && s->hitag == sprite[j].hitag &&
                    sc->hitag - T1(i) == sector[sprite[j].sectnum].hitag) break;
            if (j < 0) break;
            {
                int ds = sprite[j].sectnum, nextk;
                int32_t dx = sprite[j].x - s->x, dy = sprite[j].y - s->y;
                if (pl.cursectnum == s->sectnum) {          /* warp the player */
                    pl.posx += dx; pl.posy += dy;
                    pl.posz = sector[ds].floorz - (sc->floorz - pl.posz);
                    pl.oposz = pl.posz;
                    pl.truefz = sector[ds].floorz; pl.truecz = sector[ds].ceilingz;
                    pl.bobcounter = 0;
                    pl.bobposx += dx; pl.bobposy += dy;
                    pl.cursectnum = (int16_t)ds;
                    setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
                }
                for (k = headspritesect[s->sectnum]; k >= 0; k = nextk) {   /* and the cargo */
                    nextk = nextspritesect[k];
                    if (sprite[k].statnum == STAT_EFFECTOR || k == pl.i) continue;
                    sprite[k].x += dx; sprite[k].y += dy;
                    sprite[k].z = sector[ds].floorz - (sc->floorz - sprite[k].z);
                    changespritesect((int16_t)k, (int16_t)ds);
                }
            }
            break;
        }
        case 25:           /* pistons */
            if (T5(i)) {
                if (sc->ceilingz <= T4(i)) T5(i) = -T5(i);
                if (sc->ceilingz >= s->z) T5(i) = -T5(i);
                sc->ceilingz += (T5(i) > 0 ? -1 : 1) * (s->yvel << 4);
                if (sc->ceilingz < T4(i)) sc->ceilingz = T4(i);
                if (sc->ceilingz > s->z) sc->ceilingz = s->z;
            }
            break;
        case 31: {         /* true drop floor - ACTORS.C: toggles between the SE's z and
                            * the map floor (T2); ang 1536 = starts at the map floor */
            if (T1(i) != 1) break;
            if (T4(i) > 0) { T4(i)--; break; }
            int32_t sp = s->yvel > 0 ? s->yvel : 64, goal;
            if (T3(i) == 1) {                                   /* retract */
                goal = (s->ang != 1536) ? s->z : T2(i);
                if (klabs(sc->floorz - goal) < sp) {
                    sc->floorz = goal; T3(i) = 0; T1(i) = 0; T4(i) = s->hitag; callsound(s->sectnum, i);
                } else se31_shift(i, sgn32(goal - sc->floorz) * sp);
                break;
            }
            goal = ((s->ang & 2047) == 1536) ? s->z : T2(i);    /* extend (no snap, as Duke) */
            if (klabs(sc->floorz - goal) < sp) {
                callsound(s->sectnum, i); T1(i) = 0; T3(i) = 1; T4(i) = s->hitag;
            } else se31_shift(i, sgn32(goal - sc->floorz) * sp);
            break;
        }
        case 32: {         /* true drop ceiling - ACTORS.C */
            if (T1(i) != 1) break;
            int32_t sp = s->yvel > 0 ? s->yvel : 64, goal;
            if (T3(i) == 1) {                                   /* retract */
                goal = (s->ang != 1536) ? s->z : T2(i);
                if (klabs(sc->ceilingz - goal) < (sp << 1)) {
                    sc->ceilingz = goal; callsound(s->sectnum, i); T3(i) = 0; T1(i) = 0;
                } else sc->ceilingz += sgn32(goal - sc->ceilingz) * sp;
                break;
            }
            if ((s->ang & 2047) == 1536) {
                if (klabs(sc->ceilingz - s->z) < (sp << 1)) {
                    T1(i) = 0; T3(i) = !T3(i); callsound(s->sectnum, i); sc->ceilingz = s->z;
                } else sc->ceilingz += sgn32(s->z - sc->ceilingz) * sp;
            } else {
                if (klabs(sc->ceilingz - T2(i)) < (sp << 1)) {
                    T1(i) = 0; T3(i) = !T3(i); callsound(s->sectnum, i);
                } else sc->ceilingz -= sgn32(s->z - T2(i)) * sp;
            }
            break;
        }
        }
    }
    if (earthquaketime > 0) earthquaketime--;
}

/* ACTORS.C movetransports(), player part.  SE 7 pairs (same hitag):
 *  - "onfloorz" transporters (the SE sits on its sector's floor) are the
 *    teleporter pads: standing on one beams Duke to the partner;
 *  - the others are Duke's silent room-over-room links (the E1L1 air vent,
 *    lift shafts, ...): when Duke's eyes pass within 6144 of the SE's z he
 *    is shifted by the offset between the two SEs and keeps falling (or
 *    flying) - same velocity, same falling counter, no teleport effect;
 *  - floor transporters in lotag 1/2 sectors: diving into / surfacing
 *    from water. */
void transporter_check(void)
{
    for (int i = headspritestat[STAT_TRANSPORT]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        int ow = s->owner;
        if (T1(i) > 0) T1(i)--;
        if (ow == i || ow < 0 || s->sectnum != pl.cursectnum) continue;
        spritetype *d = &sprite[ow];
        int sectlotag = sector[s->sectnum].lotag;
        int onfloorz = T5(i);

        /* 1 = a pad / dive transporter, 2 = a silent link: the water surface
         * clamp in player_process() only stands aside for the first kind, so
         * landing in a pool that holds a silent link (E1L2) can't sink Duke
         * through the pool's floor */
        pl.on_warping_sector = onfloorz ? 1 : 2;

        if (pl.transporter_hold == 0 && pl.jumping_counter == 0) {
            if (pl.on_ground && sectlotag == 0 && onfloorz && pl.jetpack_on == 0) {
                if (s->pal == 0) spritesound(TELEPORTER, -1);
                pl.ang = d->ang;
                if (d->owner != ow) { T1(i) = 13; T1(ow) = 13; pl.transporter_hold = 13; }
                pl.bobposx = pl.posx = d->x;
                pl.bobposy = pl.posy = d->y;
                pl.oposz = pl.posz = d->z - PHEIGHT;
                pl.cursectnum = d->sectnum;
                setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
                return;
            }
        } else if (!(sectlotag == 1 && pl.on_ground == 1)) continue;

        if (onfloorz == 0 && klabs(s->z - pl.posz) < 6144) {
            if (pl.jetpack_on == 0 || (sync1.bits & (SB_JUMP | SB_CROUCH))) {
                pl.posx += d->x - s->x;
                pl.posy += d->y - s->y;
                pl.bobposx += d->x - s->x;
                pl.bobposy += d->y - s->y;
                if (pl.jetpack_on && ((sync1.bits & SB_JUMP) || pl.jetpack_on < 11)) pl.posz = d->z - 6144;
                else pl.posz = d->z + 6144;
                pl.oposz = pl.posz;
                pl.cursectnum = d->sectnum;
                setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
                return;
            }
        }

        int k = 0;
        if (onfloorz && sectlotag == 1 && pl.on_ground && pl.posz > sector[s->sectnum].floorz - (16 << 8) &&
            ((sync1.bits & SB_CROUCH) || pl.poszv > 2048)) {          /* dive */
            k = 1;
            if (pl.health > 0) spritesound(DUKE_UNDERWATER, -1);
            pl.oposz = pl.posz = sector[d->sectnum].ceilingz + (7 << 8);
            pl.posxv = 4096 - (krand() & 8192);
            pl.posyv = 4096 - (krand() & 8192);
        }
        if (onfloorz && sectlotag == 2 && pl.posz < sector[s->sectnum].ceilingz + (6 << 8)) {   /* surface */
            k = 1;
            spritesound(DUKE_GASP, -1);
            pl.oposz = pl.posz = sector[d->sectnum].floorz - (7 << 8);
            pl.jumping_toggle = 1;
            pl.jumping_counter = 0;
        }
        if (k) {
            pl.posx += d->x - s->x;
            pl.posy += d->y - s->y;
            pl.bobposx += d->x - s->x;
            pl.bobposy += d->y - s->y;
            if (d->owner != ow) pl.transporter_hold = -2;
            pl.cursectnum = d->sectnum;
            setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
            return;
        }
    }
}

void animatewalls(void)
{
    /* W_SCREENBREAK tiles are static broken screens in the original game.
     * Randomizing them made E1L4's intact map artwork jump between tiles. */
}
