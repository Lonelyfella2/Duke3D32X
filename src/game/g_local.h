/*
 * g_local.h - Duke Nukem 3D 32X gameplay internals.
 *
 * Gameplay rules, constants and data come from the GPL Duke Nukem 3D 1.5
 * source (PLAYER.C, SECTOR.C, ACTORS.C, GAME.C) and the CON scripts in the
 * user's GRP (USER.CON/DEFS.CON/GAME.CON via gen/con_*.h); the code is a
 * reduced reimplementation sized for the 32X's 256 KB SDRAM.
 */
#ifndef G_LOCAL_H
#define G_LOCAL_H

#include "game.h"
#include "names.h"
#include "soundefs.h"
#include "con_defs.h"
#include "con_actions.h"

#define TICSPERFRAME 4          /* game logic at 120/4 = 30 Hz            */
#define PHEIGHT      (38 << 8)
#define AUTO_AIM_ANGLE 48
#define GRAVITY      176        /* Duke GRAVITATIONALCONSTANT             */

enum {
    STAT_DEFAULT = 0, STAT_ACTOR = 1, STAT_ZOMBIEACTOR = 2, STAT_EFFECTOR = 3,
    STAT_PROJECTILE = 4, STAT_MISC = 5, STAT_STANDABLE = 6, STAT_LOCATOR = 7,
    STAT_ACTIVATOR = 8, STAT_TRANSPORT = 9, STAT_PLAYER = 10, STAT_FX = 11,
    STAT_FALLER = 12, STAT_DUMMYPLAYER = 13, STAT_ITEM = 14, STAT_DEAD = 15,
    STAT_CRASHSHIP = 16,            /* E1L1 intro: Duke's ship going down */
    STAT_RECON_CRASH = 17
};

/* weapon numbers come from SOUNDEFS.H (KNEE_WEAPON .. GROW_WEAPON) */
#ifndef GROW_WEAPON
#define GROW_WEAPON 11
#endif
#define MAX_WEAPONS 12

enum { INV_FIRSTAID, INV_STEROIDS, INV_HOLODUKE, INV_JETPACK, INV_HEATS, INV_SCUBA, INV_BOOTS, INV_NUM };

/* compact per-sprite state (Duke's hittype is 72 B/sprite - far too big) */
typedef struct {
    int32_t t[3];           /* temp_data 0..2: AI state / effector data  */
    int16_t t3;             /* temp_data 3 (16-bit)                      */
    int16_t timer, extra2;  /* temp_data 4/5 / misc counters             */
    int16_t floorz8;        /* last floor height >> 8 (actors)           */
} actor_t;                  /* 20 bytes */
extern actor_t act[MAXSPRITES];

typedef struct {
    int32_t posx, posy, posz, oposz;
    int32_t posxv, posyv, poszv;
    int32_t truefz, truecz;
    int16_t ang, horiz, horizoff, cursectnum;
    int16_t i;                         /* player's own sprite            */
    int16_t health, armor;
    int16_t curr_weapon, last_weapon, kickback_pic, weapon_pos, wantweapon;
    int16_t ammo[MAX_WEAPONS];
    uint16_t gotweapon;
    int16_t jumping_counter, falling_counter, hard_landing;
    uint8_t jumping_toggle, on_ground, crouching, dead_flag;
    int16_t jetpack_on;
    int16_t inv[INV_NUM];               /* inventory amounts              */
    int16_t inven_icon;
    int16_t heat_on, holoduke_on, scuba_on, steroids_on;
    uint8_t keys;                       /* bit0 blue, 1 red, 2 yellow     */
    uint8_t pal_r, pal_g, pal_b, pal_time; /* screen flash                */
    int16_t quote_num, quote_time;
    int16_t bobcounter, walking_snd_toggle;
    int16_t knee_incs, fist_incs;
    int16_t secret_rooms, max_secret_rooms, actors_killed, max_actors_killed;
    int16_t timebeforeexit, level_done;
    int16_t shrunk, grown;              /* shrinker/expander timers       */
    int16_t quake;                      /* earthquake screen shake        */
    int16_t extra_extra8;               /* damage dealt to player (accum) */
    int16_t on_warping_sector, look_ang, rotscrnang;
    int16_t last_full_weapon, weapon_sway, reloading;
    int16_t transporter_hold, return_to_center;   /* PLAYER.C */
    int16_t pickup_delay;               /* no pickups just after a hard fall */
    int32_t bobposx, bobposy;           /* position last tick (weapon sway) */
    int16_t cheat_god;
    int16_t talk_timer;                 /* one-liner cooldown             */
    int32_t level_time;
} player_t;
extern player_t pl;

/* input sync (built from the pad each tick) */
typedef struct {
    int32_t fvel, svel, avel, horz;
    uint32_t bits;
} sync_t;
#define SB_JUMP      (1u << 0)
#define SB_CROUCH    (1u << 1)
#define SB_FIRE      (1u << 2)
#define SB_AIMUP     (1u << 3)
#define SB_AIMDOWN   (1u << 4)
#define SB_RUN       (1u << 5)
#define SB_LOOKCTR   (1u << 6)
#define SB_USE       (1u << 7)
#define SB_NEXTWEAP  (1u << 8)
#define SB_PREVWEAP  (1u << 9)
#define SB_INVUSE    (1u << 10)
#define SB_INVNEXT   (1u << 11)
#define SB_ALTFIRE   (1u << 12)   /* expander (shrinker alt) */
extern sync_t sync1;

/* globals */
extern int32_t gametic, level_index, game_skill;
extern int32_t earthquaketime;
extern int32_t visibility_g;
extern int16_t camsprite;

/* g_sound.c */
void spritesound(int num, int spritenum);
void globalsound(int num);
void player_talk(int num);              /* Duke one-liner with cooldown  */
int  sound_playing(int num);
void sound_stopall(void);
void sound_ambient_update(void);

/* g_sector.c */
void sector_init_level(void);
int  setanimation(int animsect, int32_t *animptr, int32_t thegoal, int32_t thevel);
int  getanimationgoal(int32_t *animptr);
void doanimations(void);
void operatesectors(int sn, int ii);
void operateactivators(int low, int snum);
void operatemasterswitches(int low);
void activatebysector(int sect, int j);
int  checkhitswitch(int w, int switchtype);
void checkhitwall(int spr, int dawallnum, int32_t x, int32_t y, int32_t z, int atwith);
void checksectors(void);
void moveeffectors(void);
void effector_spawn(int i);
void align_warp_elevators(void);         /* SE 17: after all sprites spawn */
void animatewalls(void);
void triggerse13(int hitag);
int  isanunderoperator(int lotag);
int  isanearoperator(int lotag);
void callsound(int sn, int whatsprite);
void transporter_check(void);

/* g_actors.c */
void actors_spawn_level(void);
int  spawn_sprite(int sect, int32_t x, int32_t y, int32_t z, int pic, int shade,
                  int xr, int yr, int ang, int xvel, int zvel, int owner, int stat);
void actors_move(void);
void actor_damage(int i, int dmg, int weapon, int owner);
void hitradius(int i, int32_t r, int hp1, int hp2, int hp3, int hp4);
int  badguy(const spritetype *s);
int  badguypic(int pic);
void spawn_explosion(int sect, int32_t x, int32_t y, int32_t z, int pic, int owner);
void lotsofglass(int i, int wallnum, int n);
void spawn_projectile(int owner, int pic, int32_t x, int32_t y, int32_t z, int sect,
                      int ang, int zvel, int vel, int dmg);
int32_t actor_target_z(const spritetype *s);
int  aim(const spritetype *s, int aang, int32_t sx, int32_t sy);
void spawn_spark(int sect, int32_t x, int32_t y, int32_t z, int ang);
int  findplayer_dist(const spritetype *s);
void operaterespawns(int low);           /* RESPAWN sprites: spawn their actor */

/* g_player.c */
void player_init(int newgame);
void player_spawn_at(int32_t x, int32_t y, int32_t z, int ang, int sect);
void player_process(void);
void player_hurt(int dmg, int fromspr);
void player_quote(int q);
void player_quote_text(const char *s);
int  player_pickup(int i);
int  weapon_maxammo(int w);
int  weapon_rate(int w);
extern const char *quote_text;

/* g_main.c */
void game_level_start(int idx);
extern int16_t hud_dirty;

#define rnd(X) ((krand() >> 8) >= (255 - (X)))
#define TRAND  krand()

#endif
