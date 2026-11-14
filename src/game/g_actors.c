/*
 * g_actors.c - map sprite spawning, enemies, projectiles, explosions,
 * items and the renderer's per-sprite animation hook.
 *
 * Enemy animation frames and speeds come straight from GAME.CON (action /
 * move definitions, gen/con_actions.h); health and weapon strengths from
 * USER.CON (gen/con_defs.h); projectile speeds from PLAYER.C shoot().
 */
#include "g_local.h"

actor_t act[MAXSPRITES];

typedef struct { int16_t start, frames, views, inc, delay; } con_act_t;

enum { ATK_NONE, ATK_LASER, ATK_SHOTGUN, ATK_SPIT, ATK_COOL, ATK_RPG, ATK_HITSCAN, ATK_KAMIKAZE, ATK_BOSS, ATK_BOSS2 };

typedef struct {
    int16_t base, last;           /* tile family [base,last]            */
    int16_t strength;
    con_act_t walk, shoot, pain, dying, dead;
    int16_t speed;                /* CON walk move hvel (moves hvel/2 per tick) */
    int8_t  attack, flyer;
    int16_t clipdist;
    int16_t s_roam, s_recog, s_attack, s_pain, s_die;
    int16_t att_delay;            /* ticks between attack checks        */
} enemydef_t;

static const enemydef_t edefs[] = {
    { LIZTROOP, LIZTROOP + 99, CON_TROOPSTRENGTH, CA_ATROOPWALKING, CA_ATROOPSHOOT, CA_ATROOPFLINTCH,
      CA_ATROOPDYING, CA_ATROOPDEAD, 72, ATK_LASER, 0, 80, PRED_ROAM, PRED_RECOG, PRED_ATTACK, PRED_PAIN, PRED_DYING, 60 },
    { PIGCOP, PIGCOP + 79, CON_PIGCOPSTRENGTH, CA_APIGWALK, CA_APIGSHOOT, CA_APIGHIT, CA_APIGDYING, CA_APIGDEAD,
      72, ATK_SHOTGUN, 0, 100, PIG_ROAM, PIG_RECOG, PIG_ATTACK, PIG_PAIN, PIG_DYING, 75 },
    { LIZMAN, LIZMAN + 99, CON_LIZSTRENGTH, CA_ALIZWALKING, CA_ALIZSHOOTING, CA_ALIZSCREAM, CA_ALIZDYING,
      CA_ALIZLYINGDEAD, 72, ATK_SPIT, 0, 90, CAPT_ROAM, CAPT_RECOG, CAPT_ATTACK, CAPT_PAIN, CAPT_DYING, 45 },
    { OCTABRAIN, OCTABRAIN + 39, CON_OCTASTRENGTH, CA_AOCTAWALK, CA_AOCTASHOOT, CA_AOCTAHIT, CA_AOCTADYING,
      CA_AOCTADEAD, 96, ATK_COOL, 1, 110, OCTA_ROAM, OCTA_RECOG, OCTA_ATTACK1, OCTA_PAIN, OCTA_DYING, 70 },
    { COMMANDER, COMMANDER + 39, CON_COMMANDERSTRENGTH, CA_ACOMMBREETH, CA_ACOMMSHOOT, CA_ACOMMBREETH,
      CA_ACOMMDYING, CA_ACOMMDEAD, 128, ATK_RPG, 1, 120, COMM_ROAM, COMM_RECOG, COMM_ATTACK, COMM_PAIN, COMM_DYING, 80 },
    { RECON, RECON + 14, CON_RECONSTRENGTH, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4},
      {0, 1, 5, 0, 0}, 96, ATK_LASER, 1, 100, RECO_ROAM, RECO_RECOG, RECO_ATTACK, RECO_PAIN, RECO_DYING, 50 },
    { DRONE, DRONE + 9, CON_DRONESTRENGTH, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4},
      {0, 1, 5, 0, 0}, 128, ATK_KAMIKAZE, 1, 80, DRON_ROAM, DRON_RECOG, DRON_ATTACK1, DRON_PAIN, DRON_DYING, 30 },
    { ROTATEGUN, ROTATEGUN + 9, CON_ROTTURRETSTRENGTH, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4}, {0, 1, 5, 1, 4},
      {0, 1, 5, 1, 4}, {0, 1, 5, 0, 0}, 0, ATK_HITSCAN, 1, 64, TURR_ROAM, TURR_RECOG, TURR_ATTACK, TURR_PAIN, TURR_DYING, 40 },
    { BOSS1, BOSS1 + 79, CON_BOSS1STRENGTH, CA_ABOSS1WALK, CA_ABOSS1SHOOT, CA_ABOSS1WALK, CA_ABOSS1DYING,
      CA_ABOSS1DEAD, 208, ATK_BOSS, 0, 160, BOS1_ROAM, BOS1_RECOG, BOS1_ATTACK1, BOS1_PAIN, BOS1_DYING, 40 },
    /* Cycloid Emperor (E3L9 Stadium boss) - GAME.CON boss2code: walk (BOSS2WALKVELS),
     * RPG volley / COOLEXPLOSION1 lob, flinch, 8-frame death */
    { BOSS2, BOSS2 + 49, CON_BOSS2STRENGTH, CA_ABOSS2WALK, CA_ABOSS2SHOOT, CA_BOSS2FLINTCH, CA_ABOSS2DYING,
      CA_ABOSS2DEAD, 192, ATK_BOSS2, 0, 164, BOS2_ROAM, BOS2_RECOG, BOS2_ATTACK, BOS2_PAIN, BOS2_DYING, 36 },
    /* Overlord - GAME.CON boss3code (its own frames, not BOSS1's) */
    { BOSS3, BOSS3 + 49, CON_BOSS3STRENGTH, CA_ABOSS3WALK, CA_ABOSS3LOBBING, CA_BOSS3FLINTCH, CA_ABOSS3DYING,
      CA_ABOSS3DEAD, 208, ATK_RPG, 0, 164, BOS3_ROAM, BOS3_RECOG, BOS3_ATTACK1, BOS3_PAIN, BOS3_DYING, 45 },
    { SHARK, SHARK + 9, CON_SHARKSTRENGTH, CA_ASHARKCRUZING, CA_ASHARKATACK, CA_ASHARKCRUZING, CA_ASHARKCRUZING,
      CA_ASHARKCRUZING, 24, ATK_NONE, 1, 64, 0, 0, 0, 0, 0, 40 },
};
#define NUMEDEFS ((int)(sizeof(edefs) / sizeof(edefs[0])))

enum { AI_IDLE, AI_CHASE, AI_ATTACK, AI_PAIN, AI_DYING, AI_FROZEN, AI_SHRUNK };
#define A_STATE(i)  act[i].t[0]
#define A_FRAME(i)  act[i].t[1]
#define A_TICK(i)   act[i].t[2]
#define A_DEF(i)    act[i].extra2          /* index into edefs, -1 none */
#define A_TIMER(i)  act[i].timer
#define A_MISC(i)   act[i].t3

int badguypic(int pic)
{
    for (int k = 0; k < NUMEDEFS; k++) if (pic >= edefs[k].base && pic <= edefs[k].last) return 1;
    return pic == RAT || pic == GREENSLIME || pic == EGG;
}
int badguy(const spritetype *s) { return badguypic(s->picnum); }

static int edef_for(int pic)
{
    for (int k = 0; k < NUMEDEFS; k++) if (pic >= edefs[k].base && pic <= edefs[k].last) return k;
    return -1;
}

static int boss_base(int base) { return base == BOSS1 || base == BOSS2 || base == BOSS3; }

/* pal-0 bosses are the episode bosses (GAME.CON "ifspritepal 0" paths): they
 * can't be frozen, shrunk, expanded or gibbed, crush a player who gets within
 * 1280 units, and their death ends the episode ("endofgame 52"). */
static int is_boss0(int i)
{
    return A_DEF(i) >= 0 && boss_base(edefs[A_DEF(i)].base) && sprite[i].pal == 0;
}

static const con_act_t boss2_lob = CA_ABOSS2LOB;   /* A_MISC = 1 while lobbing */

static const con_act_t *cur_action(int i)
{
    const enemydef_t *d = &edefs[A_DEF(i)];
    switch (A_STATE(i)) {
    case AI_ATTACK: return (d->base == BOSS2 && A_MISC(i) == 1) ? &boss2_lob : &d->shoot;
    case AI_PAIN:   return &d->pain;
    case AI_DYING:  return &d->dying;
    default:        return &d->walk;
    }
}

int findplayer_dist(const spritetype *s) { return klabs(s->x - pl.posx) + klabs(s->y - pl.posy); }

/* --------------------------------------------------- generic spawning */
int spawn_sprite(int sect, int32_t x, int32_t y, int32_t z, int pic, int shade,
                 int xr, int yr, int ang, int xvel, int zvel, int owner, int stat)
{
    if (sect < 0 || sect >= numsectors) return -1;
    int i = insertsprite((int16_t)sect, (int16_t)stat);
    if (i < 0) return -1;
    spritetype *s = &sprite[i];
    memset(s, 0, sizeof(*s));
    s->x = x; s->y = y; s->z = z;
    s->picnum = (int16_t)pic; s->shade = (int8_t)shade;
    s->xrepeat = (uint8_t)xr; s->yrepeat = (uint8_t)yr;
    s->ang = (int16_t)ang; s->xvel = (int16_t)xvel; s->zvel = (int16_t)zvel;
    s->owner = (int16_t)owner; s->sectnum = (int16_t)sect; s->statnum = (int16_t)stat;
    s->extra = 0; s->lotag = 0; s->hitag = 0; s->yvel = 0; s->clipdist = 32;
    memset(&act[i], 0, sizeof(actor_t));
    A_DEF(i) = -1;
    return i;
}

void spawn_spark(int sect, int32_t x, int32_t y, int32_t z, int ang)
{
    int k = spawn_sprite(sect, x, y, z, SHOTSPARK1, -15, 10, 10, ang, 0, 0, -1, STAT_MISC);
    if (k >= 0) A_TIMER(k) = 8;
}

void spawn_explosion(int sect, int32_t x, int32_t y, int32_t z, int pic, int owner)
{
    int k = spawn_sprite(sect, x, y, z, pic, -32, 48, 48, 0, 0, 0, owner, STAT_MISC);
    if (k >= 0) { sprite[k].cstat = 128; A_TIMER(k) = 20; }
}

void lotsofglass(int i, int wallnum, int n)
{
    int32_t x, y, z; int sect;
    if (wallnum >= 0) {
        walltype *w = &wall[wallnum], *w2 = &wall[w->point2];
        x = (w->x + w2->x) >> 1; y = (w->y + w2->y) >> 1; sect = pl.cursectnum;
        int16_t s2 = (int16_t)sect; updatesector(x, y, &s2); if (s2 >= 0) sect = s2;
        z = pl.posz;
    } else if (i >= 0) { x = sprite[i].x; y = sprite[i].y; z = sprite[i].z - (16 << 8); sect = sprite[i].sectnum; }
    else return;
    n = n > 12 ? 12 : n;      /* reduced debris count for the 32X */
    for (int j = 0; j < n; j++) {
        int k = spawn_sprite(sect, x, y, z - (krand() & (16 << 8)), GLASSPIECES + (j % 3), -32, 36, 36,
                             krand() & 2047, 32 + (krand() & 63), 1024 - (krand() & 1023), -1, STAT_MISC);
        if (k >= 0) A_TIMER(k) = 40;
    }
}

/* --------------------------------------------------- level spawning */
static void spawn_map_sprite(int i)
{
    spritetype *sp = &sprite[i];
    int pic = sp->picnum;
    memset(&act[i], 0, sizeof(actor_t));
    A_DEF(i) = -1;
    act[i].floorz8 = (int16_t)(sector[sp->sectnum].floorz >> 8);
    switch (pic) {
    case SECTOREFFECTOR: effector_spawn(i); return;
    case ACTIVATOR: case ACTIVATORLOCKED:
        sp->cstat = 32768; changespritestat((int16_t)i, STAT_ACTIVATOR); return;
    case TOUCHPLATE:     /* GAME.C spawn: the plate's z is the sector's resting floor */
        act[i].t[2] = sector[sp->sectnum].floorz;
        if (sector[sp->sectnum].lotag != 1 && sector[sp->sectnum].lotag != 2) sector[sp->sectnum].floorz = sp->z;
        sp->cstat |= 32768; changespritestat((int16_t)i, STAT_STANDABLE); return;
    case MASTERSWITCH:
        sp->cstat |= 32768; sp->yvel = 0; changespritestat((int16_t)i, STAT_STANDABLE); return;
    case SEENINE: case OOZFILTER:   /* GAME.C spawn: tiny charges are invisible blast points */
        sp->shade = -16;
        if (sp->xrepeat <= 8) { sp->cstat = 32768; sp->xrepeat = sp->yrepeat = 0; }
        else sp->cstat = 1 | 256;
        sp->extra = 1;
        changespritestat((int16_t)i, STAT_STANDABLE);
        return;
    case MUSICANDSFX:
        sp->cstat = 32768; changespritestat((int16_t)i, STAT_FX); return;
    case DUKECAR:        /* E1L1: Duke's shot-down ship, see move_crashship() */
        changespritestat((int16_t)i, STAT_CRASHSHIP); return;
    case LOCATORS: case GPSPEED: case CYCLER:
        sp->cstat |= 32768; changespritestat((int16_t)i, STAT_LOCATOR); return;
    case RESPAWN:        /* GAME.C spawn(): waits in the effects list (extra 53) */
        sp->extra = 66 - 13;
        sp->cstat = 32768;
        changespritestat((int16_t)i, STAT_FX);
        return;
    case CAMERA1:
        sp->cstat = 32768 | 1;   /* invisible camera: viewscreens look through it */
        sp->extra = 1;
        changespritestat((int16_t)i, STAT_ACTOR);
        return;
    }
    if (badguypic(pic)) {
        /* difficulty: lotag = minimum skill (Duke: delete if lotag > skill) */
        if (sp->lotag > game_skill) { deletesprite((int16_t)i); return; }
        int d = edef_for(pic);
        A_DEF(i) = (int16_t)d;
        if (d >= 0) {
            sp->extra = edefs[d].strength;
            if (pic == BOSS1 && sp->pal) sp->extra = CON_BOSS1PALSTRENGTH;
            sp->clipdist = (uint8_t)edefs[d].clipdist;
            sp->picnum = edefs[d].base;    /* variants share the base frames */
            if (boss_base(edefs[d].base)) {          /* GAME.C spawn(): boss sizes */
                if (sp->pal) { sp->xrepeat = sp->yrepeat = 40; sp->clipdist = 80; }
                else         { sp->xrepeat = sp->yrepeat = 80; sp->clipdist = 164; }
            }
        } else sp->extra = 1;
        if (sp->xrepeat == 0) sp->xrepeat = sp->yrepeat = 40;
        sp->cstat |= 1 | 256;              /* blocking + hitscan */
        sp->cstat &= ~(16 | 32);
        A_STATE(i) = AI_IDLE;
        pl.max_actors_killed++;
        changespritestat((int16_t)i, STAT_ACTOR);
        return;
    }
    switch (pic) {          /* items */
    case FIRSTGUNSPRITE: case CHAINGUNSPRITE: case RPGSPRITE: case FREEZESPRITE: case SHRINKERSPRITE:
    case HEAVYHBOMB: case TRIPBOMBSPRITE: case SHOTGUNSPRITE: case DEVISTATORSPRITE:
    case AMMO: case BATTERYAMMO: case DEVISTATORAMMO: case RPGAMMO: case GROWAMMO: case CRYSTALAMMO:
    case HBOMBAMMO: case SHOTGUNAMMO: case FREEZEAMMO: case AMMOLOTS:
    case COLA: case SIXPAK: case FIRSTAID: case SHIELD: case STEROIDS: case AIRTANK: case JETPACK:
    case HEATSENSOR: case ACCESSCARD: case BOOTS: case ATOMICHEALTH: case HOLODUKE:
        if (sp->lotag > game_skill) { deletesprite((int16_t)i); return; }
        sp->cstat &= ~1;
        if (sp->xrepeat == 0) sp->xrepeat = sp->yrepeat = 32;
        changespritestat((int16_t)i, STAT_ITEM);
        return;
    case SEENINE: case OOZFILTER: case EXPLODINGBARREL: case EXPLODINGBARREL2: case FIREBARREL:
    case FIREEXT: case CRACK1: case CRACK2: case CRACK3: case CRACK4: case WOODENHORSE: case HORSEONSIDE:
    case BOX: case RUBBERCAN: case CANWITHSOMETHING: case TOILET: case STALL: case HYDRENT:
    case FANSPRITE: case WATERFOUNTAIN: case QUEBALL: case STRIPEBALL: case POCKET: case BOTTLE1: case BOTTLE2:
    case BOTTLE3: case BOTTLE4: case BOTTLE5: case BOTTLE6: case BOTTLE7: case BOTTLE8: case BOTTLE10:
    case BOTTLE11: case BOTTLE12: case BOTTLE13: case BOTTLE14: case BOTTLE15: case BOTTLE16: case BOTTLE17:
    case BOTTLE18: case BOTTLE19: case VASE: case STATUE: case STATUEFLASH: case CHAIR1: case CHAIR2:
    case CHAIR3: case POT1: case POT2: case POT3: case PIPE1: case PIPE2: case PIPE3: case PIPE4:
    case PIPE5: case PIPE6: case FETUS: case FETUSJIB: case SUSHIPLATE1: case SUSHIPLATE2: case SUSHIPLATE3:
    case SUSHIPLATE4: case SUSHIPLATE5: case WAITTOBESEATED: case HANGLIGHT: case GENERICPOLE2:
    case TRIPODCAMERA: case SCALE: case FUELPOD: case SOLARPANNEL: case ANTENNA: case FOODOBJECT16:
        if (sp->extra <= 0) sp->extra = (pic == SEENINE || pic == OOZFILTER) ? 1 : 20;
        sp->cstat |= 256;            /* shootable */
        changespritestat((int16_t)i, STAT_STANDABLE);
        return;
    }
    /* everything else is scenery */
}

void actors_spawn_level(void)
{
    pl.max_actors_killed = 0;
    for (int i = 0; i < MAXSPRITES; i++) {
        if (sprite[i].statnum >= MAXSTATUS) continue;
        if (sprite[i].picnum == SECTOREFFECTOR) spawn_map_sprite(i);
    }
    /* Duke's spawn() runs for every map sprite whatever its stored statnum */
    static uint8_t done[MAXSPRITES];
    memset(done, 0, sizeof(done));
    for (int i = 0; i < MAXSPRITES; i++) {
        if (sprite[i].statnum >= MAXSTATUS || done[i]) continue;
        if (sprite[i].picnum == SECTOREFFECTOR) continue;
        done[i] = 1;
        int st = sprite[i].statnum;
        if (st != 0) changespritestat((int16_t)i, 0);
        spawn_map_sprite(i);
    }
    /* GAME.C crane spawn: pair by hitag after all map sprites are initialized,
     * keeping the original pole position as the pickup target. */
    for (int i = 0; i < MAXSPRITES; i++) {
        spritetype *s = &sprite[i];
        if (s->statnum >= MAXSTATUS || s->picnum != CRANE) continue;
        int pole = -1;
        for (int j = 0; j < MAXSPRITES; j++)
            if (sprite[j].statnum < MAXSTATUS && sprite[j].picnum == CRANEPOLE && sprite[j].hitag == s->hitag) { pole = j; break; }
        if (pole < 0) continue;
        spritetype *p = &sprite[pole];
        act[i].t[0] = s->x; act[i].t[1] = s->y; act[i].t[2] = pole;
        act[pole].t[0] = p->x; act[pole].t[1] = p->y; act[pole].t[2] = p->sectnum;
        s->cstat |= 64 | 257; s->picnum = CRANE + 2;
        s->z = sector[s->sectnum].ceilingz + (48 << 8); s->owner = -1; s->extra = 8; s->xvel = 0;
        p->xrepeat = 48; p->yrepeat = 128; p->shade = s->shade;
        setsprite((int16_t)pole, s->x, s->y, s->z - (34 << 8));
        changespritestat((int16_t)i, STAT_STANDABLE);
    }
}

/* --------------------------------------------------------- damage */
static void kill_actor(int i, int weapon)
{
    spritetype *s = &sprite[i];
    int d = A_DEF(i);
    if (d >= 0 && edefs[d].s_die) spritesound(edefs[d].s_die, i);
    if (s->picnum == RECON) {
        /* RECON has its own crash sequence, even when hit by explosives. */
        s->extra = 0;
        s->cstat &= ~(1 | 256);
        A_TIMER(i) = 0;
        changespritestat((int16_t)i, STAT_RECON_CRASH);
        return;
    }
    pl.actors_killed++;
    if (d >= 0 && boss_base(edefs[d].base)) {
        /* GAME.CON checkboss*hitstate: bosses play their death, never gib */
        if (s->pal == 0 || rnd(64)) globalsound(DUKE_TALKTOBOSSFALL);
        A_STATE(i) = AI_DYING;
        A_FRAME(i) = 0; A_TICK(i) = 0;
        s->cstat &= ~(1 | 256);
        return;
    }
    if (weapon == RPG || weapon == RADIUSEXPLOSION || s->extra < -40) {
        /* gibbed: jibs + Duke one-liner */
        for (int k = 0; k < 6; k++) {
            int j = spawn_sprite(s->sectnum, s->x, s->y, s->z - (24 << 8) - (krand() & 0x1fff), JIBS1 + (krand() % 6),
                                 -8, 32, 32, krand() & 2047, 48 + (krand() & 63), -1024 - (krand() & 1023), -1, STAT_MISC);
            if (j >= 0) A_TIMER(j) = 60;
        }
        if (rnd(80)) player_talk(JIBBED_ACTOR1 + (krand() % 5));
        deletesprite((int16_t)i);
        return;
    }
    A_STATE(i) = AI_DYING;
    A_FRAME(i) = 0; A_TICK(i) = 0;
    s->cstat &= ~(1 | 256);
    if (rnd(40)) player_talk(rnd(128) ? DUKE_KILLED1 + 0 : JIBBED_ACTOR6);
}

/* SEENINE / OOZFILTER charge groups (ACTORS.C).  Shooting a visible charge,
 * or a master switch in its sector (shade -31), arms every charge sharing its
 * hitag (shade -32).  Each counts its lotag down by 3 per tick, then
 * detonates: the SE 13/8/18/21 effectors with that hitag fire, and charges
 * that were hit or counted down explode.  This drives the scripted
 * demolitions (E1L2's building, E3L5's set).  To spare the 32X, at most two
 * new fireballs (and explosion sounds) are spawned per tick. */
static void seenine_arm(int i, int hit)
{
    spritetype *s = &sprite[i];
    if (s->shade == -32 || s->shade == -33) return;
    if (hit) s->lotag = 0;                  /* the one that was shot goes off now */
    act[i].t3 = 1;
    for (int j = headspritestat[STAT_STANDABLE]; j >= 0; j = nextspritestat[j])
        if ((sprite[j].picnum == SEENINE || sprite[j].picnum == OOZFILTER) && sprite[j].hitag == s->hitag)
            sprite[j].shade = -32;
}

static void seenine_detonate(int i)
{
    static int32_t boom_tic = -1; static int boom_n;
    spritetype *s = &sprite[i];
    earthquaketime = 16;
    for (int j = headspritestat[STAT_EFFECTOR]; j >= 0; j = nextspritestat[j]) {
        if (sprite[j].hitag != s->hitag) continue;
        switch (sprite[j].lotag) {
        case 8:  act[j].timer = 1; break;
        case 18: if (act[j].t[0] == 0) act[j].t[0] = 1; break;
        case 21: act[j].t[0] = 1; break;
        }
    }
    triggerse13(s->hitag);
    s->z -= (32 << 8);
    if ((act[i].t3 == 1 && s->xrepeat) || s->lotag == -99) {
        if (boom_tic != gametic) { boom_tic = gametic; boom_n = 0; }
        if (boom_n++ < 2) {
            spawn_explosion(s->sectnum, s->x, s->y, s->z, EXPLOSION2, i);
            spritesound(PIPEBOMB_EXPLODE, i);
        }
        hitradius(i, CON_SEENINEBLASTRADIUS, 15, 25, 35, 50);   /* before the charge is gone */
    }
    if (s->xrepeat)
        for (int k = 0; k < 2; k++) {       /* RANDOMSCRAP x8 in Duke; 2 here */
            int j = spawn_sprite(s->sectnum, s->x, s->y, s->z, SCRAP1 + (krand() & 3), -8, 24, 24, krand() & 2047,
                                 48 + (krand() & 63), -512 - (krand() & 1023), -1, STAT_MISC);
            if (j >= 0) A_TIMER(j) = 40;
        }
    deletesprite((int16_t)i);
}

void actor_damage(int i, int dmg, int weapon, int owner)
{
    spritetype *s = &sprite[i];
    (void)owner;
    if (s->statnum == STAT_ACTOR && s->picnum == CAMERA1) {
        if (0) {   /* USER.CON: CAMERASDESTRUCTABLE NO */ s->picnum = CAMERA1 + 1; s->cstat &= ~1; }
        return;
    }
    if (s->statnum == STAT_STANDABLE) {
        switch (s->picnum) {
        case SEENINE: case OOZFILTER:          /* ACTORS.C: a hit arms the whole charge group */
            if (s->xrepeat) seenine_arm(i, 1);
            return;
        case EXPLODINGBARREL: case EXPLODINGBARREL2: case FIREBARREL: case FIREEXT:
            if (s->extra < 0) return;            /* already going off (chain reactions) */
            s->extra = -1;
            spawn_explosion(s->sectnum, s->x, s->y, s->z - (16 << 8), EXPLOSION2, i);
            spritesound(PIPEBOMB_EXPLODE, i);
            if (s->hitag) triggerse13(s->hitag);
            hitradius(i, CON_SEENINEBLASTRADIUS, 15, 25, 35, 50);   /* centred on the barrel */
            deletesprite((int16_t)i);
            return;
        case CRACK1: case CRACK2: case CRACK3: case CRACK4:
            if (weapon == RPG || weapon == RADIUSEXPLOSION || weapon == SEENINE) {
                if (s->hitag) triggerse13(s->hitag);
                deletesprite((int16_t)i);
            }
            return;
        case QUEBALL: case STRIPEBALL:
            /* pool balls roll away when kicked or shot */
            s->xvel = 64 + (dmg << 3); s->ang = (int16_t)((pl.ang) & 2047);
            spritesound(POOLBALLHIT, i);
            return;
        case TOILET: case STALL:
            s->picnum++; spritesound(GLASS_BREAKING, i); lotsofglass(i, -1, 8);
            {   int k = spawn_sprite(s->sectnum, s->x, s->y, s->z - (8 << 8), WATERFOUNTAIN + 1, 0, 32, 32, 0, 0, 0, i, STAT_MISC);
                if (k >= 0) A_TIMER(k) = 90; }
            s->statnum = STAT_STANDABLE;
            return;
        case FANSPRITE: s->picnum = FANSPRITEBROKE; s->cstat &= ~257; spritesound(GLASS_HEAVYBREAK, i); return;
        case HYDRENT: s->picnum = BROKEFIREHYDRENT; spritesound(GLASS_HEAVYBREAK, i); return;
        case WATERFOUNTAIN: s->picnum = WATERFOUNTAIN + 1; return;
        default:
            if ((s->extra -= dmg) <= 0) {
                lotsofglass(i, -1, 6);
                spritesound(GLASS_BREAKING, i);
                deletesprite((int16_t)i);
            }
            return;
        }
    }
    if (s->statnum != STAT_ACTOR || A_DEF(i) < 0) return;
    if (A_STATE(i) == AI_DYING) return;
    if (s->picnum == ROTATEGUN) { /* GAME.CON: scrap on hits; explosion, not a corpse */
        s->extra -= dmg;
        int dead = s->extra <= 0;
        for (int k = 0; k < (dead ? 15 : 4); k++) {
            int j = spawn_sprite(s->sectnum, s->x, s->y, s->z - (8 << 8), SCRAP1 + (krand() & 3),
                                 -8, 32, 32, krand() & 2047, 64 + (krand() & 63), -512 - (krand() & 1023), i, STAT_MISC);
            if (j >= 0) A_TIMER(j) = 60;
        }
        if (dead) {
            spritesound(LASERTRIP_EXPLODE, i);
            spawn_explosion(s->sectnum, s->x, s->y, s->z, EXPLOSION2, i);
            pl.actors_killed++; deletesprite((int16_t)i);
        } else { A_MISC(i) = 128; A_STATE(i) = AI_CHASE; A_FRAME(i) = 0; A_TICK(i) = 0; }
        return;
    }
    int boss0 = is_boss0(i);      /* episode bosses ignore shrink/expand/freeze */
    if (weapon == SHRINKSPARK && !boss0) { A_STATE(i) = AI_SHRUNK; A_TIMER(i) = 30 * 8; spritesound(ACTOR_SHRINKING, i); return; }
    if (weapon == GROWSPARK && !boss0) {
        s->xrepeat += 24; s->yrepeat += 24;
        if (s->xrepeat > 120) { spawn_explosion(s->sectnum, s->x, s->y, s->z - (16 << 8), EXPLOSION2, i); kill_actor(i, RPG); }
        return;
    }
    if (weapon == FREEZEBLAST && !boss0 && s->extra - dmg <= 0) {
        A_STATE(i) = AI_FROZEN; A_TIMER(i) = 30 * 6; s->pal = 1; s->extra = 0; spritesound(SOMETHINGFROZE, i);
        return;
    }
    if (A_STATE(i) == AI_FROZEN) {  /* shatter */
        lotsofglass(i, -1, 10); spritesound(GLASS_BREAKING, i); pl.actors_killed++; deletesprite((int16_t)i); return;
    }
    s->extra -= dmg;
    if (s->extra <= 0) { kill_actor(i, weapon); return; }
    const enemydef_t *d = &edefs[A_DEF(i)];
    if (A_STATE(i) == AI_IDLE) A_STATE(i) = AI_CHASE;
    if (boss_base(d->base)) {
        /* GAME.CON: a hit boss rarely flinches (ifrnd 32) and mostly shoots back */
        if (rnd(16) && d->pain.frames) { A_STATE(i) = AI_PAIN; A_FRAME(i) = 0; A_TICK(i) = 0; }
        else if (A_TIMER(i) > 6) A_TIMER(i) = 6;
    } else if (rnd(96) && d->pain.frames) { A_STATE(i) = AI_PAIN; A_FRAME(i) = 0; A_TICK(i) = 0; }
    if (d->s_pain && rnd(128)) spritesound(d->s_pain, i);
}

/* radius damage (Duke hitradius): hp1..hp4 by distance quarter */
void hitradius(int i, int32_t r, int hp1, int hp2, int hp3, int hp4)
{
    int32_t x, y, z; int sect;
    static int depth = 0;
    if (i >= 0) { x = sprite[i].x; y = sprite[i].y; z = sprite[i].z; sect = sprite[i].sectnum; }
    else { x = pl.posx; y = pl.posy; z = pl.posz; sect = pl.cursectnum; }
    (void)sect;
    if (depth > 3) return;
    depth++;
    int32_t d = klabs(x - pl.posx) + klabs(y - pl.posy) + (klabs(z - pl.posz) >> 4);
    if (i >= 0 && d < r && cansee(x, y, z - (8 << 8), sprite[i].sectnum, pl.posx, pl.posy, pl.posz, pl.cursectnum)) {
        int dmg = d < r / 3 ? hp4 : d < (2 * r) / 3 ? hp3 : hp2;
        player_hurt(dmg, i);
        pl.posxv += (pl.posx - x) << 6; pl.posyv += (pl.posy - y) << 6;
    }
    for (int stat = STAT_ACTOR; stat <= STAT_STANDABLE; stat++) {
        if (stat != STAT_ACTOR && stat != STAT_STANDABLE) continue;
        int nj;
        for (int j = headspritestat[stat]; j >= 0; j = nj) {
            nj = nextspritestat[j];
            spritetype *s = &sprite[j];
            if (j == i) continue;
            int32_t dd = klabs(s->x - x) + klabs(s->y - y) + (klabs(s->z - z) >> 4);
            if (dd >= r) continue;
            if (s->picnum == CRACK1 || s->picnum == CRACK2 || s->picnum == CRACK3 || s->picnum == CRACK4 ||
                cansee(x, y, z - (8 << 8), i >= 0 ? sprite[i].sectnum : pl.cursectnum, s->x, s->y, s->z - (16 << 8), s->sectnum)) {
                int dmg = dd < r / 3 ? hp4 : dd < (2 * r) / 3 ? hp3 : hp1;
                actor_damage(j, dmg, RADIUSEXPLOSION, i);
            }
        }
    }
    depth--;
}

/* ----------------------------------------------------- aiming */
/* Centre of Build's face-sprite hitscan bounds (including centering and ART
 * vertical offset), clipped to the visible part inside its sector. */
int32_t actor_target_z(const spritetype *s)
{
    if ((uint32_t)s->picnum >= (uint32_t)numtiles) return s->z;
    int32_t height = TILESIZY(s->picnum) * s->yrepeat * 4;
    int32_t bottom = s->z + ((s->cstat & 128) ? height / 2 : 0)
                   - tiledir[s->picnum].yoff * s->yrepeat * 4;
    int32_t top = bottom - height;
    if ((unsigned)s->sectnum < (unsigned)numsectors) {
        int32_t cz, fz;
        getzsofslope(s->sectnum, s->x, s->y, &cz, &fz);
        if (top < cz + 1) top = cz + 1;
        if (bottom > fz - 1) bottom = fz - 1;
    }
    return top < bottom ? top + (bottom - top) / 2 : s->z;
}

int aim(const spritetype *s, int aang, int32_t sx, int32_t sy)
{
    int best = -1; int32_t bestd = 0x7fffffff;
    int a = (s == &sprite[pl.i]) ? pl.ang : s->ang;
    for (int j = headspritestat[STAT_ACTOR]; j >= 0; j = nextspritestat[j]) {
        spritetype *e = &sprite[j];
        if (A_DEF(j) < 0 || e->extra <= 0 || A_STATE(j) == AI_DYING || !(e->cstat & 256)) continue;
        int32_t dx = e->x - sx, dy = e->y - sy;
        int32_t d = klabs(dx) + klabs(dy);
        if (d > 16384 || d >= bestd) continue;
        int da = ((getangle(dx, dy) - a + 1024) & 2047) - 1024;
        if (klabs(da) > aang) continue;
        if (!cansee(sx, sy, pl.posz, pl.cursectnum, e->x, e->y, actor_target_z(e), e->sectnum)) continue;
        bestd = d; best = j;
    }
    return best;
}

/* ----------------------------------------------------- projectiles */
void spawn_projectile(int owner, int pic, int32_t x, int32_t y, int32_t z, int sect,
                      int ang, int zvel, int vel, int dmg)
{
    int k = spawn_sprite(sect, x, y, z, pic, -32, 32, 32, ang, vel, zvel, owner, STAT_PROJECTILE);
    if (k < 0) return;
    sprite[k].extra = (int16_t)dmg;
    sprite[k].cstat = 128;
    switch (pic) {
    case RPG: sprite[k].xrepeat = sprite[k].yrepeat = 14; break;
    case FIRELASER: sprite[k].xrepeat = sprite[k].yrepeat = 24; break;
    case SPIT: sprite[k].xrepeat = sprite[k].yrepeat = 18; break;
    case FREEZEBLAST: sprite[k].xrepeat = sprite[k].yrepeat = 16; break;
    case SHRINKSPARK: case GROWSPARK: sprite[k].xrepeat = sprite[k].yrepeat = 30; break;
    case COOLEXPLOSION1: sprite[k].xrepeat = sprite[k].yrepeat = 16; break;
    case HEAVYHBOMB: sprite[k].xrepeat = sprite[k].yrepeat = 9; sprite[k].cstat = 0;
        A_TIMER(k) = 30 * 4; break;
    }
    A_TIMER(k) = A_TIMER(k) ? A_TIMER(k) : 30 * 6;
}

static void projectile_impact(int i, int hitsect, int hitwall, int hitspr, int32_t hx, int32_t hy, int32_t hz)
{
    spritetype *s = &sprite[i];
    int pic = s->picnum, dmg = s->extra;
    if (hitspr >= 0) {
        if (hitspr == pl.i) player_hurt(dmg, s->owner);
        else actor_damage(hitspr, (pic == RPG) ? dmg * 2 : dmg, pic, s->owner);
    }
    if (hitwall >= 0) checkhitwall(i, hitwall, hx, hy, hz, pic);
    switch (pic) {
    case RPG:
        s->x = hx; s->y = hy; s->z = hz;
        spawn_explosion(hitsect >= 0 ? hitsect : s->sectnum, hx, hy, hz, EXPLOSION2, s->owner);
        spritesound(RPG_EXPLODE, i);
        hitradius(i, CON_RPGBLASTRADIUS, dmg >> 2, dmg >> 1, (dmg * 3) >> 2, dmg);
        break;
    case COOLEXPLOSION1: case FIRELASER: case SPIT:
        spawn_spark(hitsect >= 0 ? hitsect : s->sectnum, hx, hy, hz, s->ang);
        break;
    case SHRINKSPARK: case GROWSPARK: case FREEZEBLAST:
        spawn_spark(hitsect >= 0 ? hitsect : s->sectnum, hx, hy, hz, s->ang);
        if (hitspr == pl.i && pic == SHRINKSPARK && !pl.cheat_god) pl.shrunk = 30 * 6;
        break;
    }
    deletesprite((int16_t)i);
}

/* ACTORS.C moveweapons(): a projectile reaching a parallaxed (sky) ceiling
 * just disappears - no explosion in the sky */
static int sky_ceiling_hit(int sect, int32_t x, int32_t y, int32_t z)
{
    if (!(sector[sect].ceilingstat & 1) || sector[sect].ceilingpal != 0) return 0;
    return z <= getceilzofslope((int16_t)sect, x, y) + (4 << 8);
}

static void move_projectile(int i)
{
    spritetype *s = &sprite[i];
    if (--A_TIMER(i) <= 0) {
        if (s->picnum == HEAVYHBOMB) projectile_impact(i, s->sectnum, -1, -1, s->x, s->y, s->z);
        else deletesprite((int16_t)i);
        return;
    }
    if (s->picnum == HEAVYHBOMB) {           /* thrown pipebomb: bounce + roll */
        int32_t cz, fz, ch, fh;
        int32_t x = s->x, y = s->y, z = s->z; int16_t sect = s->sectnum;
        int r = clipmove(&x, &y, &z, &sect, (s->xvel * sintable[(s->ang + 512) & 2047]) >> 0,
                         (s->xvel * sintable[s->ang & 2047]) >> 0, 64, 4 << 8, 4 << 8, CLIPMASK1);
        if (r & 32768) s->ang = (int16_t)((2048 + 1024 - s->ang) & 2047), s->xvel >>= 1;
        if (sect >= 0) { s->x = x; s->y = y; changespritesect((int16_t)i, sect); }
        getzrange(s->x, s->y, s->z - 256, s->sectnum, &cz, &ch, &fz, &fh, 64, CLIPMASK1);
        s->zvel += GRAVITY;
        s->z += s->zvel;
        if (s->z > fz - (2 << 8)) {
            s->z = fz - (2 << 8);
            if (klabs(s->zvel) > 384) { s->zvel = (int16_t)(-(s->zvel >> 1)); spritesound(PIPEBOMB_BOUNCE, i); }
            else s->zvel = 0;
            s->xvel -= s->xvel >> 3;
        }
        return;
    }
    /* stepped hitscan along this tick's motion */
    int32_t vx = (s->xvel * sintable[(s->ang + 512) & 2047]) >> 14;
    int32_t vy = (s->xvel * sintable[s->ang & 2047]) >> 14;
    int32_t vz = s->zvel;
    /* ACTORS.C moveweapons()/movesprite(): a projectile covers vel/2 per tick
     * (RPG 644 -> 322) and zvel/2 vertically; here xvel = vel/8, zvel likewise */
    int32_t nx = s->x + vx * TICSPERFRAME, ny = s->y + vy * TICSPERFRAME, nz = s->z + vz * TICSPERFRAME;
    int16_t hsect, hwall, hspr; int32_t hx, hy, hz;
    hitscangoalx = nx; hitscangoaly = ny;
    hitscan(s->x, s->y, s->z, s->sectnum, (nx - s->x) << 4, (ny - s->y) << 4, (nz - s->z) << 4,
            &hsect, &hwall, &hspr, &hx, &hy, &hz, CLIPMASK1);
    hitscangoalx = hitscangoaly = (1 << 29) - 1;
    if (hspr == s->owner) hspr = -1;
    /* the player is not a sprite in the hitscan list unless close: test directly */
    if (s->owner != pl.i) {
        int32_t dx = pl.posx - nx, dy = pl.posy - ny;
        if (klabs(dx) < 256 && klabs(dy) < 256 && nz > pl.posz - (8 << 8) && nz < pl.posz + PHEIGHT) {
            projectile_impact(i, pl.cursectnum, -1, pl.i, nx, ny, nz); return;
        }
    }
    int reached = (hsect >= 0) && (klabs(hx - s->x) + klabs(hy - s->y) < klabs(nx - s->x) + klabs(ny - s->y) - 4);
    if ((hwall >= 0 || hspr >= 0 || reached) && hsect >= 0) {
        if (hwall < 0 && hspr < 0 && sky_ceiling_hit(hsect, hx, hy, hz)) { deletesprite((int16_t)i); return; }
        projectile_impact(i, hsect, hwall, hspr, hx, hy, hz); return;
    }
    /* nothing in the way within this tick's step: fly on (hitscan reports no
     * sector when the step ends in open space) */
    int16_t ns = s->sectnum; updatesector(nx, ny, &ns);
    if (ns < 0) { deletesprite((int16_t)i); return; }
    int32_t cz, fz; getzsofslope(ns, nx, ny, &cz, &fz);
    if (nz <= cz && (sector[ns].ceilingstat & 1) && sector[ns].ceilingpal == 0) { deletesprite((int16_t)i); return; }
    if (nz <= cz || nz >= fz) { projectile_impact(i, ns, -1, -1, nx, ny, nz < cz ? cz : fz); return; }
    s->x = nx; s->y = ny; s->z = nz;
    if (ns != s->sectnum) changespritesect((int16_t)i, ns);
    if (s->picnum == RPG && (gametic & 1)) {
        int k = spawn_sprite(s->sectnum, s->x, s->y, s->z, SMALLSMOKE, -8, 24, 24, 0, 0, 0, -1, STAT_MISC);
        if (k >= 0) A_TIMER(k) = 10;
    }
}

/* ----------------------------------------------------- enemies */
static int enemy_shoot(int i)
{
    spritetype *s = &sprite[i];
    const enemydef_t *d = &edefs[A_DEF(i)];
    int32_t dx = pl.posx - s->x, dy = pl.posy - s->y;
    int32_t dist = ksqrt((uint32_t)(dx * dx + dy * dy)); if (dist < 1) dist = 1;
    int ang = getangle(dx, dy);
    int32_t sz = s->z - ((s->yrepeat * TILESIZY(s->picnum)) << 1) + (4 << 8);
    if (d->flyer) sz = s->z - (16 << 8);
    int vel;
    switch (d->attack) {
    case ATK_LASER: vel = 840;
        spawn_projectile(i, FIRELASER, s->x, s->y, sz, s->sectnum, ang + 16 - (krand() & 31),
                         (int)(((pl.posz - sz + (3 << 8)) * (vel >> 3)) / dist), vel >> 3, CON_FIRELASER_WEAPON_STRENGTH);
        break;
    case ATK_SPIT: vel = 292;
        spawn_projectile(i, SPIT, s->x, s->y, sz, s->sectnum, ang, (int)(((pl.posz - sz) * (vel >> 3)) / dist),
                         vel >> 3, CON_SPIT_WEAPON_STRENGTH);
        break;
    case ATK_COOL: vel = 348;
        spawn_projectile(i, COOLEXPLOSION1, s->x, s->y, sz, s->sectnum, ang, (int)(((pl.posz - sz) * (vel >> 3)) / dist),
                         vel >> 3, CON_COOL_EXPLOSION_STRENGTH);
        break;
    case ATK_BOSS2:
        if (dist < 10240 && rnd(128)) {      /* boss2lobbedstate: shoot COOLEXPLOSION1 */
            A_MISC(i) = 1; vel = 348;
            spawn_projectile(i, COOLEXPLOSION1, s->x, s->y, sz, s->sectnum, ang,
                             (int)(((pl.posz - sz) * (vel >> 3)) / dist), vel >> 3, CON_COOL_EXPLOSION_STRENGTH);
        } else {                              /* boss2shootenemy: shoot RPG */
            A_MISC(i) = 0; vel = 644;
            spawn_projectile(i, RPG, s->x, s->y, sz, s->sectnum, ang,
                             (int)(((pl.posz - sz) * (vel >> 3)) / dist), vel >> 3, 40);
        }
        break;
    case ATK_RPG: case ATK_BOSS: vel = 644;
        spawn_projectile(i, RPG, s->x, s->y, sz, s->sectnum, ang, (int)(((pl.posz - sz) * (vel >> 3)) / dist),
                         vel >> 3, d->attack == ATK_BOSS ? 30 : 40);
        break;
    case ATK_SHOTGUN: case ATK_HITSCAN: {
        int pellets = d->attack == ATK_SHOTGUN ? 3 : 1;
        for (int p = 0; p < pellets; p++) {
            int16_t hs, hw, hsp; int32_t hx, hy, hz;
            int sa = ang + 32 - (krand() & 63);
            int32_t zv = (((pl.posz - sz) << 8) / dist) + 128 - (krand() & 255);
            hitscan(s->x, s->y, sz, s->sectnum, sintable[(sa + 512) & 2047], sintable[sa & 2047], zv << 6,
                    &hs, &hw, &hsp, &hx, &hy, &hz, CLIPMASK1);
            /* did the shot pass the player? (player isn't in the sprite lists) */
            int32_t pdx = pl.posx - s->x, pdy = pl.posy - s->y;
            int32_t along = (pdx * (sintable[(sa + 512) & 2047] >> 4) + pdy * (sintable[sa & 2047] >> 4)) >> 10;
            int32_t hd = ksqrt((uint32_t)((hx - s->x) * (hx - s->x) + (hy - s->y) * (hy - s->y)));
            if (along > 0 && along < hd && klabs(((getangle(pdx, pdy) - sa + 1024) & 2047) - 1024) < 24 && (krand() & 3))
                player_hurt(d->attack == ATK_SHOTGUN ? 4 + (krand() & 3) : CON_BULLET_WEAPON_STRENGTH / 4, i);
            else if (hs >= 0) spawn_spark(hs, hx, hy, hz, sa);
        }
        spritesound(d->attack == ATK_SHOTGUN ? SHOTGUN_FIRE : PISTOL_FIRE, i);
        break;
    }
    case ATK_KAMIKAZE:
        if (dist < 900) {
            spawn_explosion(s->sectnum, s->x, s->y, s->z, EXPLOSION2, i);
            spritesound(RPG_EXPLODE, i);
            s->extra = 0;
            hitradius(i, 2048, 20, 30, 40, 60);
            pl.actors_killed++;
            deletesprite((int16_t)i);
            return 1;
        }
        break;
    }
    if (d->s_attack && rnd(64)) spritesound(d->s_attack, i);
    return 0;
}

static void move_enemy(int i)
{
    spritetype *s = &sprite[i];
    int d = A_DEF(i);
    if (s->picnum == CAMERA1) return;
    if (s->picnum == ROTATEGUN && A_MISC(i) > 0) { A_MISC(i)--; s->ang = (s->ang + 64) & 2047; return; }
    if (d < 0) return;
    const enemydef_t *e = &edefs[d];
    int32_t dist = findplayer_dist(s);

    if (A_STATE(i) == AI_FROZEN) {
        if (--A_TIMER(i) <= 0) { s->pal = 0; s->extra = 1; A_STATE(i) = AI_CHASE; }
        return;
    }
    if (A_STATE(i) == AI_SHRUNK) {
        if (s->xrepeat > 10) { s->xrepeat -= 2; s->yrepeat -= 2; }
        if (dist < 300 && pl.on_ground) {          /* squish! */
            spritesound(SQUISHED, i);
            int k = spawn_sprite(s->sectnum, s->x, s->y, act[i].floorz8 << 8, BLOODPOOL, 0, 16, 16, 0, 0, 0, -1, STAT_DEAD);
            (void)0;
            if (k >= 0) sprite[k].cstat = 32;
            pl.actors_killed++;
            deletesprite((int16_t)i); return;
        }
        if (--A_TIMER(i) <= 0) { s->xrepeat = s->yrepeat = 40; A_STATE(i) = AI_CHASE; }
        return;
    }
    /* GAME.CON boss*code: "ifp palive ifspritepal 0 ifpdistl 1280 { addphealth -1000
     * palfrom 63 63 }" - get too close to an episode boss and it crushes you.
     * (ifpdistl uses findplayer()'s |dx|+|dy|+|dz|/16 distance.) */
    if (A_STATE(i) != AI_DYING && !pl.dead_flag && is_boss0(i) &&
        dist + (klabs(pl.posz - s->z + (28 << 8)) >> 4) < 1280)
        player_hurt(2000, i);
    /* sleep when far (Duke: timetosleep) */
    if (dist > 24000) {
        if (A_STATE(i) == AI_IDLE) return;
        if (A_STATE(i) == AI_CHASE && !boss_base(e->base)) { A_STATE(i) = AI_IDLE; return; }
    }

    const con_act_t *a = cur_action(i);
    if (++A_TICK(i) * 4 >= (a->delay ? a->delay : 4)) {
        A_TICK(i) = 0;
        A_FRAME(i) += a->inc ? (a->inc > 0 ? 1 : -1) : 0;
        if (A_FRAME(i) >= a->frames || A_FRAME(i) < 0) {
            if (A_STATE(i) == AI_DYING) {
                int endgame = is_boss0(i);
                A_FRAME(i) = a->frames - 1;
                /* become a corpse */
                s->picnum = (int16_t)(e->base + e->dead.start);
                A_DEF(i) = -1;
                changespritestat((int16_t)i, STAT_DEAD);
                s->cstat &= ~(1 | 256);
                if (endgame && !pl.dead_flag) {     /* GAME.CON: "ifspritepal 0 endofgame 52" */
                    pl.level_done = 1;
                    pl.timebeforeexit = 52;
                }
                return;
            }
            A_FRAME(i) = 0;
            if (A_STATE(i) == AI_PAIN || A_STATE(i) == AI_ATTACK) A_STATE(i) = AI_CHASE;
        }
    }
    if (A_STATE(i) == AI_DYING) {
        if (s->z < (act[i].floorz8 << 8)) s->z = min(s->z + 512, act[i].floorz8 << 8);
        return;
    }
    if (A_STATE(i) == AI_IDLE) {
        int canseep = (dist < 18000) && ((gametic + i) % 6 == 0) &&
            cansee(s->x, s->y, s->z - (32 << 8), s->sectnum, pl.posx, pl.posy, pl.posz, pl.cursectnum);
        if (canseep && !pl.dead_flag) {
            A_STATE(i) = AI_CHASE; A_FRAME(i) = 0;
            if (e->s_recog) spritesound(e->s_recog, i);
            A_TIMER(i) = (int16_t)(e->att_delay + (krand() & 31));
        } else {
            if (e->s_roam && (krand() & 1023) == 0 && dist < 10000) spritesound(e->s_roam, i);
            return;
        }
    }
    if (pl.dead_flag) { A_STATE(i) = AI_IDLE; return; }
    int ang = getangle(pl.posx - s->x, pl.posy - s->y);
    int have_z = 0;
    int32_t cz = 0, fz = 0;
    if (A_STATE(i) == AI_CHASE) {
        /* turn toward the player */
        int da = ((ang - s->ang + 1024) & 2047) - 1024;
        s->ang = (int16_t)((s->ang + (da >> 2)) & 2047);
        if (--A_TIMER(i) <= 0 && dist < 18000 &&
            cansee(s->x, s->y, s->z - (32 << 8), s->sectnum, pl.posx, pl.posy, pl.posz, pl.cursectnum)) {
            A_TIMER(i) = (int16_t)(e->att_delay + (krand() & 31));
            if (e->attack != ATK_NONE && (dist < 12000 || e->attack == ATK_KAMIKAZE)) {
                A_STATE(i) = AI_ATTACK; A_FRAME(i) = 0; A_TICK(i) = 0;
                s->ang = (int16_t)ang;
                if (enemy_shoot(i)) return;
            }
        }
        if (e->speed && dist > 1100 && (A_STATE(i) == AI_CHASE)) {
            int32_t x = s->x, y = s->y, z = s->z - (8 << 8); int16_t sect = s->sectnum;
            /* ACTORS.C move()/movesprite(): clipmove(((hvel*sin>>14)*TICSPERFRAME)<<11)
             * = hvel/2 units per tick (a trooper walks 36/tick, Duke runs ~150) */
            int32_t v = e->speed;
            int r = clipmove(&x, &y, &z, &sect, (v * sintable[(s->ang + 512) & 2047]) >> 1,
                             (v * sintable[s->ang & 2047]) >> 1, s->clipdist << 2, 4 << 8, 24 << 8, CLIPMASK0);
            if (sect >= 0) {
                getzsofslope(sect, x, y, &cz, &fz);
                if (e->flyer || fz - s->z < (24 << 8)) {
                    s->x = x; s->y = y;
                    if (sect != s->sectnum) changespritesect((int16_t)i, sect);
                    have_z = 1;
                } else r = 1;
            } else {
                s->ang = (int16_t)((s->ang + 768) & 2047);
                r = 0;
            }
            if (r) s->ang = (int16_t)((s->ang + 512 - (krand() & 1023)) & 2047);
        }
    }
    if (e->base == ROTATEGUN) return; /* ceiling turret stays fixed in place */
    /* vertical: walkers follow the floor, flyers hover toward player height */
    if (!have_z) getzsofslope(s->sectnum, s->x, s->y, &cz, &fz);
    act[i].floorz8 = (int16_t)(fz >> 8);
    if (e->flyer) {
        int32_t goal = pl.posz - (8 << 8);
        if (goal > fz - (16 << 8)) goal = fz - (16 << 8);
        if (goal < cz + (32 << 8)) goal = cz + (32 << 8);
        s->z += (goal - s->z) >> 4;
    } else {
        if (s->z < fz) { s->zvel += GRAVITY; s->z += s->zvel; if (s->z >= fz) { s->z = fz; s->zvel = 0; } }
        else { s->z = fz; s->zvel = 0; }
    }
}

/* misc animations: sparks, explosions, debris, glass, smoke */
static void move_misc(int i)
{
    spritetype *s = &sprite[i];
    if (--A_TIMER(i) <= 0) { deletesprite((int16_t)i); return; }
    if (s->picnum == EXPLOSION2 || s->picnum == EXPLOSION2BOT) return;
    if (s->xvel || s->zvel) {
        s->x += (s->xvel * sintable[(s->ang + 512) & 2047]) >> 14;
        s->y += (s->xvel * sintable[s->ang & 2047]) >> 14;
        s->zvel += GRAVITY;
        s->z += s->zvel;
        int16_t ns = s->sectnum; updatesector(s->x, s->y, &ns);
        if (ns < 0) { deletesprite((int16_t)i); return; }
        if (ns != s->sectnum) changespritesect((int16_t)i, ns);
        int32_t fz = getflorzofslope(ns, s->x, s->y);
        if (s->z > fz) { s->z = fz; s->zvel = (int16_t)(-(s->zvel >> 2)); s->xvel >>= 1; }
    }
}

/* ACTORS.C RECON: spiral down with fireballs, then eject the pilot. */
static void move_recon_crash(int i)
{
    spritetype *s = &sprite[i];
    s->z += 1024;
    if ((++A_TIMER(i) & 3) == 0)
        spawn_explosion(s->sectnum, s->x, s->y, s->z, EXPLOSION2, i);
    s->ang = (s->ang + 96) & 2047;
    int32_t x = s->x, y = s->y, z = s->z;
    int16_t sect = s->sectnum;
    int hit = clipmove(&x, &y, &z, &sect,
                       (128 * sintable[(s->ang + 512) & 2047]) >> 1,
                       (128 * sintable[s->ang & 2047]) >> 1,
                       s->clipdist << 2, 4 << 8, 4 << 8, CLIPMASK0);
    if (sect >= 0) {
        s->x = x; s->y = y;
        if (sect != s->sectnum) changespritesect((int16_t)i, sect);
    }
    int32_t floorz = getflorzofslope(s->sectnum, s->x, s->y);
    if (!hit && sect >= 0 && s->z <= floorz) return;
    if (s->z > floorz) s->z = floorz;
    for (int n = 0; n < 16; n++) {
        int k = spawn_sprite(s->sectnum, s->x, s->y, s->z - (8 << 8),
                             SCRAP1 + (krand() & 3), -8, 32, 32,
                             krand() & 2047, 48 + (krand() & 63),
                             -1024 - (krand() & 1023), i, STAT_MISC);
        if (k >= 0) A_TIMER(k) = 40;
    }
    spawn_explosion(s->sectnum, s->x, s->y, s->z, EXPLOSION2, i);
    spritesound(LASERTRIP_EXPLODE, i);
    int k = spawn_sprite(s->sectnum, s->x, s->y, s->z, PIGCOP, 0, 40, 40,
                         s->ang, 0, 0, i, STAT_DEFAULT);
    if (k >= 0) {
        spawn_map_sprite(k);
        if (sprite[k].statnum == STAT_ACTOR) A_STATE(k) = AI_CHASE;
    }
    pl.actors_killed++;
    deletesprite((int16_t)i);
}

/* E1L1 opens just after Duke's ship (DUKECAR) has been shot down; the map
 * leaves it hanging in the sky beside the roof.  Here it goes down: as soon
 * as it comes into Duke's view it dives away to his left, burning and
 * trailing smoke, and blows up below the roofline. */
static void move_crashship(int i)
{
    spritetype *s = &sprite[i];
    actor_t *a = &act[i];
    if (a->t[0] == 0) {
        int view = getangle(s->x - pl.posx, s->y - pl.posy);
        int da = ((view - pl.ang + 1024) & 2047) - 1024;
        if (klabs(da) > 300) return;                 /* not on screen yet */
        a->t[0] = 1;
        a->t[1] = (view - 384) & 2047;               /* screen-left and away from Duke */
        a->timer = 0;
        a->t[2] = s->z;
    }
    int t = ++a->timer;
    int hv = 48 + (t >> 1);                          /* xy units per tick */
    int zv = t * 28; if (zv > 1536) zv = 1536;       /* falls faster and faster */
    int c = sintable[(a->t[1] + 512) & 2047], sn = sintable[a->t[1] & 2047];
    s->x += (hv * c) >> 14;
    s->y += (hv * sn) >> 14;
    s->z += zv;
    s->shade = (int8_t)((krand() & 7) - 6);          /* fire-lit flicker */
    int16_t ns = s->sectnum; updatesector(s->x, s->y, &ns);
    if (ns >= 0 && ns != s->sectnum) changespritesect((int16_t)i, ns);
    /* the tail is up and behind the direction of travel */
    int32_t tx = s->x - ((900 * c) >> 14), ty = s->y - ((900 * sn) >> 14), tz = s->z - (24 << 8);
    if (t & 1) {
        int k = spawn_sprite(s->sectnum, tx, ty, tz, BURNING + (krand() & 1), -32, 96, 96, 0, 0, 0, -1, STAT_MISC);
        if (k >= 0) A_TIMER(k) = 3;
    }
    if ((t % 3) == 0) {
        int k = spawn_sprite(s->sectnum, tx, ty, tz, SMALLSMOKE, 0, 176, 176, 0, 0, 0, -1, STAT_MISC);
        if (k >= 0) { A_TIMER(k) = 36; act[k].t[2] = 36; }
    }
    if (s->z > a->t[2] + (300 << 8) || t > 150) {    /* well below the roof by now */
        int k = spawn_sprite(s->sectnum, s->x, s->y, s->z, EXPLOSION2, -32, 200, 200, 0, 0, 0, -1, STAT_MISC);
        if (k >= 0) { sprite[k].cstat = 128; A_TIMER(k) = 20; }
        spritesound(PIPEBOMB_EXPLODE, i);
        deletesprite((int16_t)i);
    }
}

/* SECTOR.C operaterespawns(): an activator with this lotag starts the
 * RESPAWN sprites carrying it - a transporter sparkle, and 12 ticks later
 * the actor named by the RESPAWN's hitag (E1L5's Battlelord arrives so) */
void operaterespawns(int low)
{
    for (int i = headspritestat[STAT_FX]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        if (s->picnum != RESPAWN || s->lotag != low || s->extra != 66 - 13) continue;
        int k = spawn_sprite(s->sectnum, s->x, s->y, s->z - (32 << 8), TRANSPORTERSTAR, -127, 48, 64,
                             s->ang, 0, 0, -1, STAT_MISC);
        if (k >= 0) { sprite[k].cstat = 128 | 2; A_TIMER(k) = 14; act[k].t[2] = 14; }
        s->extra = 66 - 12;
    }
}

/* ACTORS.C movefx() RESPAWN: count up to 66, then spawn and disappear */
static void move_respawns(void)
{
    int nexti;
    for (int i = headspritestat[STAT_FX]; i >= 0; i = nexti) {
        nexti = nextspritestat[i];
        spritetype *s = &sprite[i];
        if (s->picnum != RESPAWN) continue;
        if (s->extra == 66) {
            int pic = s->hitag;
            int k = spawn_sprite(s->sectnum, s->x, s->y, s->z, pic, 0, 40, 40, s->ang, 0, 0, -1, STAT_DEFAULT);
            if (k >= 0) {
                /* GAME.C spawn(): only bosses inherit the RESPAWN's palette */
                sprite[k].pal = (pic >= BOSS1 && pic < BOSS3 + 50) ? s->pal : 0;
                sprite[k].lotag = 0;               /* no skill filter for spawned actors */
                spawn_map_sprite(k);
                if (k < MAXSPRITES && sprite[k].statnum == STAT_ACTOR && A_DEF(k) >= 0) {
                    A_STATE(k) = AI_CHASE;          /* timetosleep = 0: awake at once */
                    if (edefs[A_DEF(k)].s_recog) spritesound(edefs[A_DEF(k)].s_recog, k);
                }
            }
            deletesprite((int16_t)i);
        } else if (s->extra > 66 - 13) s->extra++;
    }
}

/* items: bob + pickup */
static void move_items(void)
{
    int nexti;
    if (pl.pickup_delay > 0) return;
    for (int i = headspritestat[STAT_ITEM]; i >= 0; i = nexti) {
        nexti = nextspritestat[i];
        spritetype *s = &sprite[i];
        if (s->cstat & 32768) continue;
        if (klabs(s->x - pl.posx) < 640 && klabs(s->y - pl.posy) < 640 && klabs(s->z - (pl.posz + PHEIGHT)) < (48 << 8))
            if (player_pickup(i)) deletesprite((int16_t)i);
    }
}

/* pool balls and other rollers */
/* A compact adaptation of ACTORS.C's crane cycle.  Pole state retains the
 * map pickup sector/coordinates; crane state retains its home coordinates. */
static void move_crane(int i)
{
    spritetype *s = &sprite[i]; actor_t *a = &act[i];
    int pi = a->t[2];
    if (pi < 0 || pi >= MAXSPRITES || sprite[pi].statnum >= MAXSTATUS) return;
    actor_t *p = &act[pi]; int state = a->t3;
    int32_t homez = sector[s->sectnum].ceilingz + (48 << 8);
    if (state == 0) {
        if (pl.cursectnum == p->t[2] && pl.on_ground && !pl.dead_flag) {
            a->t3 = 1; s->ang = getangle(p->t[0] - s->x, p->t[1] - s->y);
        }
    } else if (state == 1 || state == 6) {
        int32_t tx = state == 1 ? p->t[0] : a->t[0];
        int32_t ty = state == 1 ? p->t[1] : a->t[1];
        int32_t dx = tx - s->x, dy = ty - s->y;
        if (klabs(dx) < 128 && klabs(dy) < 128) {
            setsprite((int16_t)i, tx, ty, s->z); a->t3++; s->xvel = 0;
        } else {
            if (s->xvel < 184) s->xvel += 8;
            s->ang = getangle(dx, dy); s->picnum = CRANE + 1;
            setsprite((int16_t)i, s->x + ((s->xvel * sintable[(s->ang + 512) & 2047]) >> 14),
                      s->y + ((s->xvel * sintable[s->ang & 2047]) >> 14), s->z);
        }
    } else if (state == 2 || state == 7) {
        s->z += 1536;
        if (sector[s->sectnum].floorz - s->z < (64 << 8) && s->picnum > CRANE) s->picnum--;
        if (state == 2 && sector[s->sectnum].floorz - s->z < 5120) a->t3++;
        if (state == 7 && s->picnum == CRANE) { s->owner = -1; a->t3++; }
    } else if (state == 3) {
        if (++s->picnum >= CRANE + 2) {
            if (pl.cursectnum == p->t[2] && pl.on_ground && !pl.dead_flag) { s->owner = -2; pl.ang = (s->ang + 1024) & 2047; }
            a->timer = 0; a->t3++;
        }
    } else if (state == 4) { if (++a->timer > 10) a->t3++; }
    else if (state == 5 || state == 8) {
        if (state == 8 && sector[s->sectnum].floorz - s->z > 8192 && s->picnum < CRANE + 2) s->picnum++;
        if (s->z < homez) { a->t3++; s->xvel = 0; } else s->z -= 1536;
    } else a->t3 = 0;
    setsprite((int16_t)pi, s->x, s->y, s->z - (34 << 8));
    if (s->owner == -2) {
        pl.posx = s->x - (sintable[(pl.ang + 512) & 2047] >> 6);
        pl.posy = s->y - (sintable[pl.ang & 2047] >> 6); pl.posz = s->z + (2 << 8);
        pl.posxv = pl.posyv = pl.poszv = 0;
        setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
        pl.cursectnum = sprite[pl.i].sectnum;
    }
}

static void move_standables(void)
{
    int16_t boom[24]; int nboom = 0;
    for (int i = headspritestat[STAT_STANDABLE]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        int pic = s->picnum;
        if ((pic == QUEBALL || pic == STRIPEBALL) && s->xvel > 0) {
            int32_t x = s->x, y = s->y, z = s->z - 256; int16_t sect = s->sectnum;
            int r = clipmove(&x, &y, &z, &sect, (s->xvel * sintable[(s->ang + 512) & 2047]) >> 1,
                             (s->xvel * sintable[s->ang & 2047]) >> 1, 24, 4 << 8, 4 << 8, CLIPMASK1);
            if (r) { s->ang = (int16_t)((s->ang + 1024 + (krand() & 255) - 128) & 2047); spritesound(POOLBALLHIT, i); }
            if (sect >= 0) { s->x = x; s->y = y; if (sect != s->sectnum) changespritesect((int16_t)i, sect); }
            s->xvel -= (s->xvel >> 4) + 1;
            if (s->xvel < 0) s->xvel = 0;
        } else if (pic == MASTERSWITCH && s->yvel == 1) {   /* ACTORS.C: delayed trigger */
            if (--s->hitag <= 0) {
                int sect = s->sectnum;
                operatesectors(sect, i);
                for (int j = headspritesect[sect]; j >= 0; j = nextspritesect[j]) {
                    if (sprite[j].statnum == STAT_EFFECTOR) {
                        switch (sprite[j].lotag) {
                        case 2: case 21: case 31: case 32: case 36: act[j].t[0] = 1; break;
                        case 3: act[j].timer = 1; break;
                        }
                    } else if (sprite[j].statnum == STAT_STANDABLE &&
                               (sprite[j].picnum == SEENINE || sprite[j].picnum == OOZFILTER))
                        sprite[j].shade = -31;            /* set the charges off */
                }
                s->yvel = 2;          /* KILLIT: spent (never re-armed) */
            }
        } else if (pic >= CRANE && pic <= CRANE + 2) {
            move_crane(i);
        } else if (pic == TOUCHPLATE) {
            actor_t *t = &act[i];
            int sect = s->sectnum;
            if (t->t[1] == 1 && s->hitag >= 0) {           /* move the plate's floor */
                int32_t x = sector[sect].floorz, e = sector[sect].extra;
                if (t->t3 == 1) {
                    if (x >= t->t[2]) t->t[1] = 0;
                    else { sector[sect].floorz += e; if (pl.cursectnum == sect) pl.posz += e; }
                } else {
                    if (x <= s->z) { sector[sect].floorz = s->z; t->t[1] = 0; }
                    else { sector[sect].floorz -= e; if (pl.cursectnum == sect) pl.posz -= e; }
                }
                continue;
            }
            if (t->extra2 == 1) continue;                  /* used up */
            if (pl.cursectnum == sect && (pl.on_ground || s->ang == 512)) {
                if (t->t[0] == 0) {
                    t->t[0] = 1; t->t[1] = 1; t->t3 = (int16_t)!t->t3;
                    operatemasterswitches(s->lotag);
                    operateactivators(s->lotag, 0);
                    if (s->hitag > 0 && --s->hitag == 0) t->extra2 = 1;
                }
            } else t->t[0] = 0;
            if (t->t[1] == 1)
                for (int j = headspritestat[STAT_STANDABLE]; j >= 0; j = nextspritestat[j])
                    if (j != i && sprite[j].picnum == TOUCHPLATE && sprite[j].lotag == s->lotag) {
                        act[j].t[1] = 1; act[j].t3 = t->t3;
                    }
        } else if (pic == SEENINE || pic == OOZFILTER) {
            if (s->shade == -31) seenine_arm(i, 0);
            else if (s->shade == -32) {
                if (s->lotag > 0) { s->lotag -= 3; if (s->lotag <= 0) s->lotag = -99; }
                else s->shade = -33;
            } else if (s->shade == -33 && nboom < 24) boom[nboom++] = (int16_t)i;
        }
    }
    for (int k = 0; k < nboom; k++) {
        int i = boom[k];
        if (sprite[i].statnum == STAT_STANDABLE && sprite[i].shade == -33 &&
            (sprite[i].picnum == SEENINE || sprite[i].picnum == OOZFILTER))
            seenine_detonate(i);
    }
}

void actors_move(void)
{
    int nexti;
    for (int i = headspritestat[STAT_ACTOR]; i >= 0; i = nexti) { nexti = nextspritestat[i]; move_enemy(i); }
    for (int i = headspritestat[STAT_PROJECTILE]; i >= 0; i = nexti) { nexti = nextspritestat[i]; move_projectile(i); }
    for (int i = headspritestat[STAT_MISC]; i >= 0; i = nexti) { nexti = nextspritestat[i]; move_misc(i); }
    for (int i = headspritestat[STAT_RECON_CRASH]; i >= 0; i = nexti) { nexti = nextspritestat[i]; move_recon_crash(i); }
    for (int i = headspritestat[STAT_CRASHSHIP]; i >= 0; i = nexti) { nexti = nextspritestat[i]; move_crashship(i); }
    move_respawns();
    move_items();
    move_standables();
}

/* ------------------------------------------- renderer animation hook */
/* Called by both renderers for each visible sprite (read-only!). */
int game_tsprite(int snum, int16_t *picnum, uint16_t *cstat, int8_t *shade)
{
    spritetype *s = &sprite[snum];
    (void)shade;
    int pic = s->picnum;
    if (s->statnum == STAT_ACTOR && A_DEF(snum) >= 0) {
        const con_act_t *a = cur_action(snum);
        int frame = (int)A_FRAME(snum);
        if (frame < 0) frame = 0;
        if (frame >= a->frames) frame = a->frames - 1;
        if (A_STATE(snum) == AI_FROZEN) frame = 0;
        pic = edefs[A_DEF(snum)].base + a->start + frame * (a->views > 0 ? a->views : 1);
        if (a->views == 5) {
            int k = getangle(s->x - rview.posx, s->y - rview.posy);
            k = (((s->ang + 3072 + 128 - k) & 2047) >> 8) & 7;
            if (k > 4) { k = 8 - k; *cstat |= 4; } else *cstat &= ~4;
            pic += k;
        }
    } else if (s->statnum == STAT_RECON_CRASH) {
        int k = (((s->ang + 3072 + 128 - getangle(s->x - rview.posx, s->y - rview.posy)) & 2047) / 170);
        if (k > 6) { k = 12 - k; *cstat |= 4; } else *cstat &= ~4;
        pic = RECON + k;
    } else if (s->statnum == STAT_MISC && (pic == EXPLOSION2)) {
        pic = EXPLOSION2 + (20 - act[snum].timer);
        if (pic > EXPLOSION2 + 20) pic = EXPLOSION2 + 20;
    } else if (s->statnum == STAT_MISC && pic == SMALLSMOKE) {   /* 4 frames over its life */
        int life = act[snum].t[2] > 0 ? act[snum].t[2] : 10;
        int f = ((life - act[snum].timer) * 4) / life;
        pic = SMALLSMOKE + (f < 0 ? 0 : f > 3 ? 3 : f);
    } else if (pic == APLAYER) {                  /* Duke in mirrors / HoloDuke */
        int k = getangle(s->x - rview.posx, s->y - rview.posy);
        k = (((s->ang + 3072 + 128 - k) & 2047) >> 8) & 7;
        if (k > 4) { k = 8 - k; *cstat |= 4; } else *cstat &= ~4;
        pic = APLAYER + k;
    } else if (pic == SECTOREFFECTOR) return 0;
    *picnum = (int16_t)pic;
    return 1;
}
