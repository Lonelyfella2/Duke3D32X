/*
 * g_player.c - Duke's movement physics (PLAYER.C processinput constants),
 * weapons/ballistics (PLAYER.C shoot), inventory and pickups.
 */
#include "g_local.h"
#ifdef HOSTDEBUG
#include <stdio.h>
#endif

player_t pl;
sync_t sync1;
const char *quote_text;

static int32_t dukefriction = CON_RUNNINGSPEED;     /* 53200 = 0xCFD0 */

/* weapon table: tile, fire interval (ticks), ammo per shot, CON strength */
typedef struct { int16_t tile, rate, ammo_item, maxammo; } weapdef_t;
static const weapdef_t wdefs[MAX_WEAPONS] = {
    { KNEE,       10, 0, 0 },
    { FIRSTGUN,    5, AMMO, CON_MAXPISTOLAMMO },
    { SHOTGUN,    24, SHOTGUNAMMO, CON_MAXSHOTGUNAMMO },
    { CHAINGUN,    3, BATTERYAMMO, CON_MAXCHAINGUNAMMO },
    { RPGGUN,     22, RPGAMMO, CON_MAXRPGAMMO },
    { HANDTHROW,  20, HBOMBAMMO, CON_MAXHANDBOMBAMMO },
    { SHRINKER,   20, CRYSTALAMMO, CON_MAXSHRINKERAMMO },
    { DEVISTATOR,  6, DEVISTATORAMMO, CON_MAXDEVISTATORAMMO },
    { TRIPBOMB,   20, TRIPBOMBSPRITE, CON_MAXTRIPBOMBAMMO },
    { FREEZE,      5, FREEZEAMMO, CON_MAXFREEZEAMMO },
    { HANDREMOTE, 10, 0, 0 },
    { SHRINKER,   20, GROWAMMO, CON_MAXSHRINKERAMMO },
};

int weapon_maxammo(int w) { return ((unsigned)w < MAX_WEAPONS) ? wdefs[w].maxammo : 0; }
int weapon_rate(int w) { return ((unsigned)w < MAX_WEAPONS) ? wdefs[w].rate : 1; }

void player_quote(int q)
{
    const char *s = engine_quote(q);
    if (s && *s) { quote_text = s; pl.quote_time = 30 * 4; }
}
void player_quote_text(const char *s) { quote_text = s; pl.quote_time = 30 * 4; }

void player_init(int newgame)
{
    if (newgame) {
        memset(&pl, 0, sizeof(pl));
        pl.health = CON_MAXPLAYERHEALTH;
        pl.gotweapon = (1 << KNEE_WEAPON) | (1 << PISTOL_WEAPON);
        pl.ammo[PISTOL_WEAPON] = 48;
        pl.curr_weapon = PISTOL_WEAPON;
    } else {
        /* carry weapons/ammo/inventory between levels, reset transient state */
        pl.keys = 0;
        pl.jetpack_on = 0; pl.heat_on = 0; pl.scuba_on = 0; pl.holoduke_on = 0;
        pl.dead_flag = 0;
        if (pl.health <= 0) pl.health = CON_MAXPLAYERHEALTH;
    }
    pl.secret_rooms = pl.actors_killed = 0;
    pl.level_done = 0; pl.timebeforeexit = 0; pl.level_time = 0;
    pl.horiz = 100; pl.horizoff = 0;
    pl.kickback_pic = 0;
    pl.shrunk = pl.grown = 0;
    pl.pal_time = 0;
}

void player_spawn_at(int32_t x, int32_t y, int32_t z, int ang, int sect)
{
    {   /* stand on the start sector's floor (no fall damage at spawn) */
        int16_t s2 = (int16_t)sect;
        updatesector(x, y, &s2);
        if (s2 >= 0) {
            sect = s2;
            int32_t fz = getflorzofslope(s2, x, y), cz = getceilzofslope(s2, x, y);
            z = fz - (40 << 8);
            if (z < cz + (4 << 8)) z = cz + (4 << 8);
        }
    }
    pl.posx = x; pl.posy = y; pl.posz = z; pl.oposz = z;
    pl.ang = (int16_t)ang; pl.cursectnum = (int16_t)sect;
    pl.posxv = pl.posyv = pl.poszv = 0;
    pl.on_ground = 0; pl.jumping_counter = 0; pl.falling_counter = 0;
    pl.hard_landing = 0; pl.return_to_center = 0; pl.transporter_hold = 0; pl.on_warping_sector = 0;
    pl.pickup_delay = 0;
    pl.horiz = 100; pl.horizoff = 0;
    pl.bobposx = x; pl.bobposy = y; pl.bobcounter = 0; pl.weapon_sway = 1024;
    int i = insertsprite((int16_t)sect, STAT_PLAYER);
    pl.i = (int16_t)i;
    if (i >= 0) {
        memset(&sprite[i], 0, sizeof(spritetype));
        sprite[i].x = x; sprite[i].y = y; sprite[i].z = z + PHEIGHT;
        sprite[i].picnum = APLAYER; sprite[i].sectnum = (int16_t)sect; sprite[i].statnum = STAT_PLAYER;
        sprite[i].cstat = 1 | 256 | 32768;         /* invisible to our own camera */
        sprite[i].clipdist = 64; sprite[i].xrepeat = sprite[i].yrepeat = 42;
        sprite[i].extra = pl.health; sprite[i].owner = (int16_t)i;
    }
    pl.max_secret_rooms = 0;
    for (int s = 0; s < numsectors; s++) if (sector[s].lotag == 32767) pl.max_secret_rooms++;
}

/* PLAYER.C: a hard fall takes health directly (armour does not help) */
static void fall_damage(int dmg)
{
    if (pl.dead_flag || pl.cheat_god || dmg <= 0) return;
    pl.health -= dmg;
    hud_dirty = 2;
    pl.pal_r = 63; pl.pal_g = 0; pl.pal_b = 0; pl.pal_time = 12;
    if (pl.health <= 0) {
        pl.health = 0;
        pl.dead_flag = 1;
        globalsound(SQUISHED);
    } else {
        globalsound(DUKE_LAND);
        globalsound(DUKE_LAND_HURT);
    }
    /* Duke is stunned for a moment: keep the damage on the HUD before an
     * item he landed on (E1L1's vent has a six-pack at the bottom) heals it */
    pl.pickup_delay = 15;
}

void player_hurt(int dmg, int fromspr)
{
    (void)fromspr;
    if (pl.dead_flag || pl.cheat_god || dmg <= 0) return;
    if (pl.armor > 0) {
        int a = dmg >> 1;          /* armour soaks half (Duke: shield absorbs) */
        if (a > pl.armor) a = pl.armor;
        pl.armor -= a; dmg -= a;
    }
    pl.health -= dmg;
    pl.pal_r = 63; pl.pal_g = 0; pl.pal_b = 0; pl.pal_time = 12;
    hud_dirty = 2;
    if (pl.health <= 0) {
        pl.health = 0;
        pl.dead_flag = 1;
        globalsound(DUKE_KILLED1 + (krand() % 3));
    } else if (dmg > CON_YELLHURTSOUNDSTRENGTH / 4 && !sound_playing(DUKE_LONGTERM_PAIN)) {
        globalsound(DUKE_LONGTERM_PAIN + (krand() & 3));
    } else if (!sound_playing(DUKE_GRUNT) && rnd(128)) globalsound(DUKE_GRUNT);
}

static int add_ammo(int weapon, int amount)
{
    int mx = wdefs[weapon].maxammo;
    if (pl.ammo[weapon] >= mx) return 0;
    pl.ammo[weapon] = (int16_t)min(mx, pl.ammo[weapon] + amount);
    return 1;
}

static void add_weapon(int weapon)
{
    int had = pl.gotweapon & (1 << weapon);
    pl.gotweapon |= (uint16_t)(1 << weapon);
    if (!had) { pl.wantweapon = (int16_t)weapon; }
    if (weapon == SHRINKER_WEAPON) pl.gotweapon |= (1 << GROW_WEAPON);
}

/* returns 1 if the item was taken (quote numbers as in GAME.CON) */
int player_pickup(int i)
{
    spritetype *s = &sprite[i];
    int got = 0, snd = DUKE_GET, talk = 0;
    switch (s->picnum) {
    case AMMO:        got = add_ammo(PISTOL_WEAPON, CON_PISTOLAMMOAMOUNT); if (got) player_quote(65); break;
    case AMMOLOTS:    got = add_ammo(PISTOL_WEAPON, 48); if (got) player_quote(65); break;
    case SHOTGUNAMMO: got = add_ammo(SHOTGUN_WEAPON, CON_SHOTGUNAMMOAMOUNT); if (got) player_quote(69); break;
    case BATTERYAMMO: got = add_ammo(CHAINGUN_WEAPON, CON_CHAINGUNAMMOAMOUNT); if (got) player_quote(63); break;
    case RPGAMMO:     got = add_ammo(RPG_WEAPON, 5); if (got) player_quote(64); break;
    case HBOMBAMMO:   got = add_ammo(HANDBOMB_WEAPON, 1); if (got) { add_weapon(HANDBOMB_WEAPON); player_quote(55); } break;
    case CRYSTALAMMO: got = add_ammo(SHRINKER_WEAPON, CON_CRYSTALAMMOAMOUNT); if (got) player_quote(78); break;
    case GROWAMMO:    got = add_ammo(GROW_WEAPON, 5); if (got) player_quote(123); break;
    case DEVISTATORAMMO: got = add_ammo(DEVISTATOR_WEAPON, CON_DEVISTATORAMMOAMOUNT); if (got) player_quote(14); break;
    case FREEZEAMMO:  got = add_ammo(FREEZE_WEAPON, CON_FREEZEAMMOAMOUNT); if (got) player_quote(66); break;
    case FIRSTGUNSPRITE: add_weapon(PISTOL_WEAPON); add_ammo(PISTOL_WEAPON, 12); got = 1; break;
    case SHOTGUNSPRITE: add_weapon(SHOTGUN_WEAPON); add_ammo(SHOTGUN_WEAPON, 10); got = 1; talk = DUKE_GETWEAPON2; player_quote(57); break;
    case CHAINGUNSPRITE: add_weapon(CHAINGUN_WEAPON); add_ammo(CHAINGUN_WEAPON, 50); got = 1; talk = DUKE_GETWEAPON6; player_quote(54); break;
    case RPGSPRITE: add_weapon(RPG_WEAPON); add_ammo(RPG_WEAPON, 5); got = 1; talk = DUKE_GETWEAPON3; player_quote(56); break;
    case FREEZESPRITE: add_weapon(FREEZE_WEAPON); add_ammo(FREEZE_WEAPON, 25); got = 1; talk = DUKE_GETWEAPON4; player_quote(59); break;
    case SHRINKERSPRITE: add_weapon(SHRINKER_WEAPON); add_ammo(SHRINKER_WEAPON, 10); got = 1; talk = DUKE_GETWEAPON1; player_quote(60); break;
    case DEVISTATORSPRITE: add_weapon(DEVISTATOR_WEAPON); add_ammo(DEVISTATOR_WEAPON, 15); got = 1; talk = DUKE_GETWEAPON6; player_quote(87); break;
    case HEAVYHBOMB: add_weapon(HANDBOMB_WEAPON); add_ammo(HANDBOMB_WEAPON, 1); got = 1; player_quote(55); break;
    case TRIPBOMBSPRITE: add_weapon(TRIPBOMB_WEAPON); add_ammo(TRIPBOMB_WEAPON, 1); got = 1; player_quote(58); break;
    case COLA:
        if (pl.health < CON_MAXPLAYERHEALTH) { pl.health = (int16_t)min(CON_MAXPLAYERHEALTH, pl.health + 10); got = 1; player_quote(61); }
        break;
    case SIXPAK:
        if (pl.health < CON_MAXPLAYERHEALTH) { pl.health = (int16_t)min(CON_MAXPLAYERHEALTH, pl.health + 30); got = 1; player_quote(62); }
        break;
    case ATOMICHEALTH:
        if (pl.health < CON_MAXPLAYERATOMICHEALTH) {
            pl.health = (int16_t)min(CON_MAXPLAYERATOMICHEALTH, pl.health + 50); got = 1; snd = GETATOMICHEALTH;
            player_quote(19);
            if (pl.health > 150 && rnd(128)) talk = DUKE_GOTHEALTHATLOW;
        }
        break;
    case SHIELD:
        if (pl.armor < 100) { pl.armor = (int16_t)min(100, pl.armor + CON_SHIELD_AMOUNT); got = 1; player_quote(38); }
        break;
    case FIRSTAID: pl.inv[INV_FIRSTAID] = CON_MAXPLAYERHEALTH; pl.inven_icon = INV_FIRSTAID; got = 1; player_quote(3); break;
    case STEROIDS: pl.inv[INV_STEROIDS] = CON_STEROID_AMOUNT; pl.inven_icon = INV_STEROIDS; got = 1; player_quote(37); break;
    case HOLODUKE: pl.inv[INV_HOLODUKE] = CON_HOLODUKE_AMOUNT; pl.inven_icon = INV_HOLODUKE; got = 1; player_quote(51); break;
    case JETPACK: pl.inv[INV_JETPACK] = CON_JETPACK_AMOUNT; pl.inven_icon = INV_JETPACK; got = 1; player_quote(41); break;
    case HEATSENSOR: pl.inv[INV_HEATS] = CON_HEAT_AMOUNT; pl.inven_icon = INV_HEATS; got = 1; player_quote(101); break;
    case AIRTANK: pl.inv[INV_SCUBA] = CON_SCUBA_AMOUNT; pl.inven_icon = INV_SCUBA; got = 1; player_quote(39); break;
    case BOOTS: pl.inv[INV_BOOTS] = CON_BOOT_AMOUNT; pl.inven_icon = INV_BOOTS; got = 1; player_quote(6); break;
    case ACCESSCARD:
        pl.keys |= (s->pal == 0) ? 1 : (s->pal == 21) ? 2 : 4;
        got = 1; player_quote(43);
        break;
    }
    if (got) {
        globalsound(snd);
        if (talk && rnd(160)) player_talk(talk);
        pl.pal_r = 0; pl.pal_g = 32; pl.pal_b = 0; pl.pal_time = 6;
        hud_dirty = 2;
    }
    return got;
}

/* --------------------------------------------------------- shooting */
static void hitscan_attack(int pic, int dmg, int spread, int count)
{
    int32_t sx = pl.posx, sy = pl.posy, sz = pl.posz + (2 << 8);
    int target = aim(&sprite[pl.i], AUTO_AIM_ANGLE, sx, sy);
    for (int n = 0; n < count; n++) {
        int sa = pl.ang;
        int32_t zvel;
        if (target >= 0) {
            spritetype *t = &sprite[target];
            sa = getangle(t->x - sx, t->y - sy);
            /* hitscan's XY direction is a 14-bit unit vector. Manhattan
             * distance made diagonal shots fall short vertically. */
            int32_t d = dmulscale14(t->x - sx, sintable[(sa + 512) & 2047],
                                     t->y - sy, sintable[sa & 2047]);
            if (d < 1) d = 1;
            int64_t vz = ((int64_t)(actor_target_z(t) - sz) * 256) / d;
            zvel = (int32_t)max(-32000000, min(32000000, vz));
        } else zvel = (100 - pl.horiz - pl.horizoff) << 5;
        if (spread) { sa += spread - (krand() & (2 * spread - 1)); zvel += 128 - (krand() & 255); }
        int16_t hs, hw, hsp; int32_t hx, hy, hz;
        hitscan(sx, sy, sz, pl.cursectnum, sintable[(sa + 512) & 2047], sintable[sa & 2047], zvel * 64,
                &hs, &hw, &hsp, &hx, &hy, &hz, CLIPMASK1);
#ifdef HOSTDEBUG
        fprintf(stderr, "  hitscan: target %d sa %d zvel %d -> sect %d wall %d spr %d (%d,%d,%d)\n", target, sa, zvel, hs, hw, hsp, hx, hy, hz);
#endif
        if (hs < 0) continue;
        if (pic == KNEE && (klabs(hx - sx) + klabs(hy - sy)) > 1024) continue;
        if (hsp >= 0 && hsp != pl.i) {
            actor_damage(hsp, dmg + (pic == SHOTSPARK1 ? (int)((uint32_t)krand() % 6) : (krand() & 3)), pic, pl.i);
            if (sprite[hsp].statnum == STAT_ACTOR) {
                int k = spawn_sprite(hs, hx, hy, hz, BLOOD, -8, 24, 24, 0, 0, 0, -1, STAT_MISC);
                if (k >= 0) act[k].timer = 8;
            } else spawn_spark(hs, hx, hy, hz, sa);
        } else {
            if (pic != KNEE) spawn_spark(hs, hx, hy, hz, sa);
            if (hw >= 0) {
                checkhitwall(pl.i, hw, hx, hy, hz, pic);
                if (pic == KNEE || wallx[hw].lotag) checkhitswitch(hw, 0);
                if (n == 0 && pic != KNEE && (krand() & 255) < 48) spritesound(PISTOL_RICOCHET, -1);
            }
        }
    }
}

static void fire_projectile(int pic, int vel, int dmg, int angofs)
{
    int32_t sz = pl.posz + (4 << 8);
    int zvel = (100 - pl.horiz - pl.horizoff) * 98 / 8;
    int target = aim(&sprite[pl.i], 48, pl.posx, pl.posy);
    int ang = pl.ang + angofs;
    if (target >= 0) {
        spritetype *t = &sprite[target];
        ang = getangle(t->x - pl.posx, t->y - pl.posy) + angofs;
        int32_t d = dmulscale14(t->x - pl.posx, sintable[(ang + 512) & 2047],
                                 t->y - pl.posy, sintable[ang & 2047]);
        if (d < 1) d = 1;
        int64_t vz = ((int64_t)(actor_target_z(t) - sz) * (vel >> 3)) / d;
        zvel = (int)max(-32768, min(32767, vz));
    }
    spawn_projectile(pl.i, pic, pl.posx + (sintable[(ang + 512) & 2047] >> 8), pl.posy + (sintable[ang & 2047] >> 8),
                     sz, pl.cursectnum, ang, zvel, vel >> 3, dmg);
}

static int weapon_ammo_ok(int w)
{
    if (w == KNEE_WEAPON || w == HANDREMOTE_WEAPON) return 1;
    return pl.ammo[w] > 0;
}

static void select_weapon(int dir)
{
    int w = pl.curr_weapon;
    for (int n = 0; n < MAX_WEAPONS; n++) {
        w = (w + dir + MAX_WEAPONS) % MAX_WEAPONS;
        if (w == HANDREMOTE_WEAPON || w == GROW_WEAPON) continue;
        if ((pl.gotweapon & (1 << w)) && weapon_ammo_ok(w) && tile_present(wdefs[w].tile)) {
            pl.curr_weapon = (int16_t)w; pl.kickback_pic = 0; hud_dirty = 2;
            globalsound(SELECT_WEAPON);
            return;
        }
    }
}

static void fire_weapon(int alt)
{
    int w = pl.curr_weapon;
    if (alt && w == SHRINKER_WEAPON && (pl.gotweapon & (1 << GROW_WEAPON)) && pl.ammo[GROW_WEAPON] > 0) w = GROW_WEAPON;
    if (!weapon_ammo_ok(w)) { select_weapon(1); return; }
    switch (w) {
    case KNEE_WEAPON:
        hitscan_attack(KNEE, CON_KNEE_WEAPON_STRENGTH, 0, 1);
        globalsound(KICK_HIT);
        break;
    case PISTOL_WEAPON:
        hitscan_attack(SHOTSPARK1, CON_PISTOL_WEAPON_STRENGTH, 16, 1);
        globalsound(PISTOL_FIRE);
        pl.ammo[w]--;
        if ((pl.ammo[w] % 12) == 0 && pl.ammo[w] > 0) { pl.reloading = 20; globalsound(EJECT_CLIP); }
        break;
    case SHOTGUN_WEAPON:
        hitscan_attack(SHOTGUN, CON_SHOTGUN_WEAPON_STRENGTH, 32, 7);    /* 7 pellets as in PLAYER.C */
        globalsound(SHOTGUN_FIRE);
        pl.ammo[w]--;
        break;
    case CHAINGUN_WEAPON:
        hitscan_attack(CHAINGUN, CON_CHAINGUN_WEAPON_STRENGTH, 16, 1);
        globalsound(CHAINGUN_FIRE);
        pl.ammo[w]--;
        break;
    case RPG_WEAPON:
        fire_projectile(RPG, 644, CON_RPG_WEAPON_STRENGTH, 0);
        globalsound(RPG_SHOOT);
        pl.ammo[w]--;
        break;
    case DEVISTATOR_WEAPON:
        fire_projectile(RPG, 644, CON_RPG_WEAPON_STRENGTH / 4, ((gametic & 1) ? 16 : -16));
        globalsound(RPG_SHOOT);
        pl.ammo[w]--;
        break;
    case FREEZE_WEAPON:
        fire_projectile(FREEZEBLAST, 644, CON_FREEZETHROWER_WEAPON_STRENGTH, 0);
        globalsound(CAT_FIRE);
        pl.ammo[w]--;
        break;
    case SHRINKER_WEAPON:
        fire_projectile(SHRINKSPARK, 400, 0, 0);
        globalsound(SHRINKER_FIRE);
        pl.ammo[w]--;
        break;
    case GROW_WEAPON:
        fire_projectile(GROWSPARK, 400, 0, 0);
        globalsound(EXPANDERSHOOT);
        pl.ammo[w]--;
        break;
    case HANDBOMB_WEAPON: {
        int k = spawn_sprite(pl.cursectnum, pl.posx, pl.posy, pl.posz + (6 << 8), HEAVYHBOMB, -16, 9, 9, pl.ang,
                             (pl.ammo[w] > 0) ? 32 : 0, (100 - pl.horiz) * 20 - 512, pl.i, STAT_PROJECTILE);
        if (k >= 0) { sprite[k].extra = CON_HANDBOMB_WEAPON_STRENGTH; act[k].timer = 32767; sprite[k].cstat = 0; }
        pl.ammo[w]--;
        pl.last_weapon = HANDBOMB_WEAPON;
        pl.gotweapon |= (1 << HANDREMOTE_WEAPON);
        pl.curr_weapon = HANDREMOTE_WEAPON;
        break;
    }
    case HANDREMOTE_WEAPON:
        for (int i = headspritestat[STAT_PROJECTILE]; i >= 0; i = nextspritestat[i])
            if (sprite[i].picnum == HEAVYHBOMB && sprite[i].owner == pl.i) act[i].timer = 1;
        pl.curr_weapon = (pl.ammo[HANDBOMB_WEAPON] > 0) ? HANDBOMB_WEAPON : PISTOL_WEAPON;
        break;
    case TRIPBOMB_WEAPON: {
        int16_t hs, hw, hsp; int32_t hx, hy, hz;
        hitscan(pl.posx, pl.posy, pl.posz, pl.cursectnum, sintable[(pl.ang + 512) & 2047], sintable[pl.ang & 2047], 0,
                &hs, &hw, &hsp, &hx, &hy, &hz, CLIPMASK1);
        if (hw >= 0 && klabs(hx - pl.posx) + klabs(hy - pl.posy) < 768) {
            int k = spawn_sprite(hs, hx, hy, hz, TRIPBOMB, 0, 4, 5, (getangle(wall[wall[hw].point2].x - wall[hw].x,
                                 wall[wall[hw].point2].y - wall[hw].y) - 512) & 2047, 0, 0, pl.i, STAT_STANDABLE);
            if (k >= 0) { sprite[k].cstat = 16 | 256; sprite[k].extra = 1; }
            globalsound(LASERTRIP_ONWALL);
            pl.ammo[w]--;
        }
        break;
    }
    }
    pl.kickback_pic = 1;
    hud_dirty = 2;
}

/* -------------------------------------------------------- physics */
void player_process(void)
{
    int32_t cz, hz, fz, lz, i, k;
    spritetype *s = &sprite[pl.i];
    int psect = pl.cursectnum;
    int psectlotag = sector[psect].lotag & 0x7fff;
    uint32_t sb = sync1.bits;

    pl.level_time++;
    if (pl.talk_timer > 0) pl.talk_timer--;
    if (pl.pickup_delay > 0) pl.pickup_delay--;
    if (pl.quote_time > 0) pl.quote_time--;
    if (pl.pal_time > 0) pl.pal_time--;
    if (pl.reloading > 0) pl.reloading--;

    if (pl.dead_flag) {
        if (pl.posz < sector[pl.cursectnum].floorz - (8 << 8)) pl.posz += 512;
        if (pl.horiz < 160) pl.horiz += 4;
        return;
    }

    /* Crane carries the player; normal gravity and movement must not fight it. */
    for (int ci = headspritestat[STAT_STANDABLE]; ci >= 0; ci = nextspritestat[ci])
        if (sprite[ci].picnum >= CRANE && sprite[ci].picnum <= CRANE + 2 && sprite[ci].owner == -2) return;

    /* PLAYER.C: transporter hold-off */
    if (pl.transporter_hold > 0) {
        if (--pl.transporter_hold == 0 && pl.on_warping_sector) pl.transporter_hold = 2;
    }
    if (pl.transporter_hold < 0) pl.transporter_hold++;

    getzrange(pl.posx, pl.posy, pl.posz, (int16_t)psect, &cz, &hz, &fz, &lz, 163, CLIPMASK0);
    getzsofslope((int16_t)psect, pl.posx, pl.posy, &pl.truecz, &pl.truefz);
    int32_t truefdist = klabs(pl.posz - pl.truefz);

    i = 40;          /* eye height above floor, in z>>8 units */
    if (psectlotag != 2) pl.scuba_on = 0;  /* PLAYER.C: the tank is only worn underwater */
    if (psectlotag == 2) {                 /* ---- underwater ---- */
        pl.jumping_counter = 0;
        if (sb & SB_JUMP) { if (pl.poszv > 0) pl.poszv = 0; pl.poszv -= 348; if (pl.poszv < -(256 * 6)) pl.poszv = -(256 * 6); }
        else if (sb & SB_CROUCH) { if (pl.poszv < 0) pl.poszv = 0; pl.poszv += 348; if (pl.poszv > (256 * 6)) pl.poszv = 256 * 6; }
        else {
            if (pl.poszv < 0) { pl.poszv += 256; if (pl.poszv > 0) pl.poszv = 0; }
            if (pl.poszv > 0) { pl.poszv -= 256; if (pl.poszv < 0) pl.poszv = 0; }
        }
        if (pl.poszv > 2048) pl.poszv >>= 1;
        pl.posz += pl.poszv;
        if (pl.posz > fz - (15 << 8)) pl.posz += ((fz - (15 << 8)) - pl.posz) >> 1;
        if (pl.posz < cz + (4 << 8)) { pl.posz = cz + (4 << 8); pl.poszv = 0; }
        if (pl.inv[INV_SCUBA] > 0) { pl.inv[INV_SCUBA]--; pl.scuba_on = 1; }
        else if ((pl.level_time & 31) == 0) player_hurt(5, -1);   /* drowning */
    } else if (pl.jetpack_on) {           /* ---- jetpack ---- */
        pl.on_ground = 0; pl.jumping_counter = 0; pl.hard_landing = 0; pl.falling_counter = 0;
        pl.poszv = 0;
        if (pl.jetpack_on < 11) { pl.jetpack_on++; pl.posz -= (pl.jetpack_on << 7); }
        else if (!sound_playing(DUKE_JETPACK_IDLE) && (pl.level_time & 15) == 0) globalsound(DUKE_JETPACK_IDLE);
        k = (sb & SB_RUN) ? 4096 : 2048;
        if (sb & SB_JUMP) { pl.posz -= k; }
        if (sb & SB_CROUCH) { pl.posz += k; }
        if (pl.posz > fz - (i << 8)) pl.posz += ((fz - (i << 8)) - pl.posz) >> 1;
        if (pl.posz < cz + (18 << 8)) pl.posz = cz + (18 << 8);
        if ((pl.level_time & 3) == 0 && --pl.inv[INV_JETPACK] <= 0) {
            pl.jetpack_on = 0; pl.inv[INV_JETPACK] = 0; globalsound(DUKE_JETPACK_OFF); hud_dirty = 2;
        }
    } else {                               /* ---- walking ---- */
        if (psectlotag == 1 && pl.on_ground) i = 34;   /* wading */
        if (sb & SB_CROUCH) { if (pl.on_ground) pl.posz += (2048 + 768); pl.crouching = 1; }
        else pl.crouching = 0;
        if (pl.posz < (fz - (i << 8))) {   /* falling */
            if ((sb & 3) == 0 && pl.on_ground && (sector[psect].floorstat & 2) && pl.posz >= (fz - (i << 8) - (16 << 8)))
                pl.posz = fz - (i << 8);
            else {
                pl.on_ground = 0;
                pl.poszv += (GRAVITY + 80);
                if (pl.poszv >= (4096 + 2048)) pl.poszv = (4096 + 2048);
                if (pl.poszv > 2400 && pl.falling_counter < 255) {
                    pl.falling_counter++;
                    if (pl.falling_counter == 38) globalsound(DUKE_SCREAM);
                }
                if ((pl.posz + pl.poszv) >= (fz - (i << 8))) {      /* hit the ground */
                    if (sector[pl.cursectnum].lotag != 1) {
                        if (pl.falling_counter > 62) fall_damage(999);
                        else if (pl.falling_counter > 9) fall_damage(pl.falling_counter - (krand() & 3));
                        else if (pl.poszv > 2048) globalsound(DUKE_LAND);
                    }
                }
            }
        } else {
            pl.falling_counter = 0;
            if (psectlotag != 1 && psectlotag != 2 && pl.on_ground == 0 && pl.poszv > (6144 >> 1))
                pl.hard_landing = (int16_t)(pl.poszv >> 10);
            pl.on_ground = 1;
            if (i == 40) {
                k = ((fz - (i << 8)) - pl.posz) >> 1;
                if (klabs(k) < 256) k = 0;
                pl.posz += k;
                pl.poszv -= 768;
                if (pl.poszv < 0) pl.poszv = 0;
            } else if (pl.jumping_counter == 0) {
                pl.posz += ((fz - (i << 7)) - pl.posz) >> 1;
                if (pl.on_warping_sector != 1 && pl.posz > fz - (16 << 8)) { pl.posz = fz - (16 << 8); pl.poszv >>= 1; }
            }
            pl.on_warping_sector = 0;
            if ((sb & SB_JUMP) == 0 && pl.jumping_toggle == 1) pl.jumping_toggle = 0;
            else if ((sb & SB_JUMP) && pl.jumping_toggle == 0) {
                if (pl.jumping_counter == 0 && (fz - cz) > (56 << 8)) {
                    pl.jumping_counter = 1;
                    pl.jumping_toggle = 1;
                }
            }
        }
        if (pl.jumping_counter) {
            if ((sb & SB_JUMP) == 0 && pl.jumping_toggle == 1) pl.jumping_toggle = 0;
            if (pl.jumping_counter < (1024 + 256)) {
                if (psectlotag == 1 && pl.jumping_counter > 768) { pl.jumping_counter = 0; pl.poszv = -512; }
                else {
                    pl.poszv -= (sintable[(2048 - 128 + pl.jumping_counter) & 2047]) / 12;
                    pl.jumping_counter += 180;
                    pl.on_ground = 0;
                }
            } else { pl.jumping_counter = 0; pl.poszv = 0; }
        }
        pl.posz += pl.poszv;
        if (pl.posz < (cz + (4 << 8))) {
            pl.jumping_counter = 0;
            if (pl.poszv < 0) pl.posxv = pl.posyv = 0;
            pl.poszv = 128;
            pl.posz = cz + (4 << 8);
        }
    }

    /* PLAYER.C: look up/down, centring and the hard-landing dip.  A hard
     * landing (or the centre key) starts return_to_center, which pulls the
     * horizon back to 100 and snaps it there: after a fall Duke always ends
     * up looking straight ahead.  The dip is half of Duke's for the 32X. */
    if ((sb & SB_LOOKCTR) || pl.hard_landing) pl.return_to_center = 9;
    if (sb & (SB_AIMUP | SB_AIMDOWN)) {
        pl.horiz += (sb & SB_AIMUP) ? 6 : -6;
        if (!pl.hard_landing) pl.return_to_center = 0;      /* the player is aiming */
    } else if (pl.return_to_center > 0) {
        pl.return_to_center--;
        pl.horiz += 33 - (pl.horiz / 3);
    }
    if (pl.hard_landing > 0) {
        pl.hard_landing--;
        pl.horiz -= (pl.hard_landing << 3);
    }
    if (!(sb & (SB_AIMUP | SB_AIMDOWN)) && pl.horiz > 95 && pl.horiz < 105) pl.horiz = 100;
    if (pl.horiz > 200) pl.horiz = 200;
    if (pl.horiz < 0) pl.horiz = 0;
    if (pl.horizoff > 0) pl.horizoff -= (pl.horizoff >> 3) + 1;
    else if (pl.horizoff < 0) pl.horizoff += ((-pl.horizoff) >> 3) + 1;

    /* PLAYER.C: the weapon sway follows the distance walked last tick */
    if (s->xvel < 32 || pl.on_ground == 0 || pl.bobcounter == 1024) {
        if ((pl.weapon_sway & 2047) > (1024 + 96)) pl.weapon_sway -= 96;
        else if ((pl.weapon_sway & 2047) < (1024 - 96)) pl.weapon_sway += 96;
        else pl.weapon_sway = 1024;
    } else pl.weapon_sway = pl.bobcounter;
    {
        int32_t dx = pl.posx - pl.bobposx, dy = pl.posy - pl.bobposy;
        /* more than 1024 units in one tick = a teleport/warp, not walking */
        s->xvel = (klabs(dx) < 1024 && klabs(dy) < 1024) ? (int16_t)ksqrt((uint32_t)(dx * dx + dy * dy)) : 0;
    }
    if (pl.on_ground) pl.bobcounter += s->xvel >> 1;
    pl.bobposx = pl.posx; pl.bobposy = pl.posy;

    /* PLAYER.C: no moving or turning while hitting the ground hard or just
     * after a teleporter */
    int frozen = pl.hard_landing || pl.transporter_hold > 2;

    /* turning */
    if (sync1.avel && !frozen) { pl.ang = (int16_t)((pl.ang + (sync1.avel << 1)) & 2047); }

    /* horizontal movement: Duke's fvel/svel are already world-space */
    int32_t doubvel = TICSPERFRAME;
    if (pl.steroids_on > 0) doubvel <<= 1;
    if (frozen) { doubvel = 0; pl.posxv = pl.posyv = 0; }
    if (sync1.fvel || sync1.svel || pl.posxv || pl.posyv) {
        pl.posxv += ((sync1.fvel * doubvel) << 6);
        pl.posyv += ((sync1.svel * doubvel) << 6);
        int32_t fr = dukefriction;
        if (pl.on_ground && (sb & SB_CROUCH)) fr = dukefriction - 0x2000;
        else if (psectlotag == 2) fr = dukefriction - 0x1400;
        pl.posxv = mulscale16(pl.posxv, fr);
        pl.posyv = mulscale16(pl.posyv, fr);
        if (klabs(pl.posxv) < 2048 && klabs(pl.posyv) < 2048) pl.posxv = pl.posyv = 0;
        if (pl.shrunk) { pl.posxv = mulscale16(pl.posxv, 40000); pl.posyv = mulscale16(pl.posyv, 40000); }
    }
    int32_t flordist = (psectlotag == 1) ? (4 << 8) : (20 << 8);
    int16_t cs = pl.cursectnum;
    clipmove(&pl.posx, &pl.posy, &pl.posz, &cs, pl.posxv, pl.posyv, 164, (4 << 8), flordist, CLIPMASK0);
    if (cs >= 0) pl.cursectnum = cs;
    if (pushmove(&pl.posx, &pl.posy, &pl.posz, &cs, 164, (4 << 8), (4 << 8), CLIPMASK0) >= 0 && cs >= 0)
        pl.cursectnum = cs;
    setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
    s->extra = pl.health;

    /* squished by a closing door / under-operators */
    if (klabs(fz - cz) < (48 << 8) && !(sector[pl.cursectnum].lotag & 0x8000) &&
        (isanunderoperator(sector[pl.cursectnum].lotag) || isanearoperator(sector[pl.cursectnum].lotag)))
        activatebysector(pl.cursectnum, pl.i);

    /* hazards */
    int fpic = sector[pl.cursectnum].floorpicnum;
    if (!pl.cheat_god && pl.level_time > 90 && pl.on_ground && truefdist <= PHEIGHT + (16 << 8) &&
        (fpic == FLOORSLIME || fpic == HURTRAIL || fpic == FLOORPLASMA)) {
        if (pl.inv[INV_BOOTS] > 0) { if ((pl.level_time & 3) == 0) pl.inv[INV_BOOTS]--; }
        else if (rnd(32)) { player_hurt(1 + (krand() & 3), -1); if (fpic == HURTRAIL) globalsound(SHORT_CIRCUIT); }
    }

    /* inventory timers (the status bar shows the item's % and ON/OFF) */
    if ((pl.level_time & 15) == 0 && (pl.jetpack_on || pl.heat_on || pl.scuba_on || pl.steroids_on)) hud_dirty = 2;
    if (pl.heat_on && (pl.level_time & 3) == 0 && --pl.inv[INV_HEATS] <= 0) { pl.heat_on = 0; pl.inv[INV_HEATS] = 0; hud_dirty = 2; }
    if (pl.steroids_on > 0 && --pl.steroids_on == 0) pl.inv[INV_STEROIDS] = 0;
    if (pl.shrunk > 0) pl.shrunk--;

    /* inventory use */
    if (sb & SB_INVNEXT) {
        for (int n = 1; n <= INV_NUM; n++) {
            int c = (pl.inven_icon + n) % INV_NUM;
            if (pl.inv[c] > 0) { pl.inven_icon = (int16_t)c; hud_dirty = 2; break; }
        }
    }
    if (sb & SB_INVUSE) {
        switch (pl.inven_icon) {
        case INV_FIRSTAID:
            if (pl.inv[INV_FIRSTAID] > 0 && pl.health < CON_MAXPLAYERHEALTH) {
                int need = CON_MAXPLAYERHEALTH - pl.health;
                if (need > pl.inv[INV_FIRSTAID]) need = pl.inv[INV_FIRSTAID];
                pl.health += need; pl.inv[INV_FIRSTAID] -= need;
                globalsound(DUKE_USEMEDKIT);
            }
            break;
        case INV_STEROIDS:
            if (pl.inv[INV_STEROIDS] > 0 && !pl.steroids_on) { pl.steroids_on = 30 * 13; globalsound(DUKE_TAKEPILLS); }
            break;
        case INV_JETPACK:
            if (pl.inv[INV_JETPACK] > 0) {
                pl.jetpack_on = !pl.jetpack_on;
                globalsound(pl.jetpack_on ? DUKE_JETPACK_ON : DUKE_JETPACK_OFF);
            }
            break;
        case INV_HEATS:
            if (pl.inv[INV_HEATS] > 0) { pl.heat_on = !pl.heat_on; globalsound(NITEVISION_ONOFF); }
            break;
        case INV_HOLODUKE:
            if (pl.inv[INV_HOLODUKE] > 0) {
                if (pl.holoduke_on >= 0 && pl.holoduke_on < MAXSPRITES && sprite[pl.holoduke_on].picnum == APLAYER &&
                    pl.holoduke_on != pl.i && sprite[pl.holoduke_on].statnum < MAXSTATUS) {
                    deletesprite(pl.holoduke_on); pl.holoduke_on = -1;
                } else {
                    int k = spawn_sprite(pl.cursectnum, pl.posx, pl.posy, pl.posz + PHEIGHT, APLAYER, -8, 42, 36,
                                         pl.ang, 0, 0, pl.i, STAT_DUMMYPLAYER);
                    if (k >= 0) { sprite[k].cstat = 1; pl.holoduke_on = (int16_t)k; }
                }
                globalsound(TELEPORTER);
            }
            break;
        }
        hud_dirty = 2;
    }

    /* weapons */
    if (pl.wantweapon) {
        if (tile_present(wdefs[pl.wantweapon].tile)) { pl.curr_weapon = pl.wantweapon; hud_dirty = 2; }
        pl.wantweapon = 0;
    }
    if (sb & SB_NEXTWEAP) select_weapon(1);
    if (sb & SB_PREVWEAP) select_weapon(-1);
    if (pl.kickback_pic) {
        pl.kickback_pic++;
        int rate = wdefs[pl.curr_weapon].rate;
        if (pl.kickback_pic > rate) pl.kickback_pic = 0;
    }
    if ((sb & (SB_FIRE | SB_ALTFIRE)) && pl.kickback_pic == 0 && pl.reloading == 0) fire_weapon((sb & SB_ALTFIRE) != 0);

    checksectors();
    transporter_check();
}
