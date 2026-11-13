/*
 * hostgame.c - runs the real game code on the desktop with a scripted pad,
 * dumping frames (PPM) - a fast oracle for gameplay before the emulator.
 *
 *   hostgame blob.bin script.txt outdir
 * script: "wait N" (frames), "hold BTN..", "release BTN..", "press BTN N", "shot NAME", "log"
 */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "game.h"
#include "g_local.h"

static pixel_t fb[2][96 + 32 + 192][160];   /* view rows | HUD (native) | private text lines */
static uint8_t txt_on[2][192];
static int cur;
static uint32_t ticks, pad;
static int staged_recon = -1;
static uint16_t cram[256];
static FILE *script;
static const char *outdir;
static int frames_to_wait;
static jmp_buf done_jmp;

static rstats_t mirror_seen = { .mirror_wall = -1 };
void plat_render_begin(void)
{
    rview.frame = &fb[cur][0][0]; rview.pitch = 160; rview.xdim = 160; rview.ydim = 96;
    if (mirror_seen.mirror_wall >= 0) render_mirror(&mirror_seen);
    render_frame_m(0, 79);
    render_frame_s(80, 159);
    render_merge_mirror(&mirror_seen, &rstats_m, &rstats_s);
}
void plat_render_end(void) {}
void plat_render_view(void) { plat_render_begin(); plat_render_end(); }
pixel_t *plat_hud_base(void) { return &fb[cur][96][0]; }
pixel_t *plat_view_base(void) { return &fb[cur][0][0]; }
uint8_t *plat_text_span(int y0, int y1)
{
    for (int y = y0; y < y1; y++) plat_text_line(y);
    return plat_text_line(y0);
}
uint8_t *plat_text_line(int sy)
{
    if ((unsigned)sy >= 224u) return 0;
    if (sy >= 192) return (uint8_t *)&fb[cur][96][0] + (sy - 192) * 320;
    uint8_t *dst = (uint8_t *)&fb[cur][128 + sy][0];
    if (!txt_on[cur][sy]) { memcpy(dst, &fb[cur][sy >> 1][0], 320); txt_on[cur][sy] = 1; }
    return dst;
}
uint8_t *plat_screen_line(int sy)
{
    if ((unsigned)sy >= 224u) return 0;
    if (sy >= 192) return (uint8_t *)&fb[cur][96][0] + (sy - 192) * 320;
    txt_on[cur][sy] = 1;
    return (uint8_t *)&fb[cur][128 + sy][0];
}
void plat_frame_begin(void) { memset(txt_on[cur], 0, sizeof txt_on[cur]); }
uint32_t plat_ticks(void) { return ticks; }
int plat_pal(void) { return 0; }
void plat_set_palette(const uint16_t *c) { memcpy(cram, c, 512); }
void plat_set_palette_async(const uint16_t *c) { plat_set_palette(c); }
void plat_telemetry(uint16_t a, uint16_t b) { (void)a; (void)b; }
void plat_sound_play(int id, int vol, int pan, int32_t pitch, int loop, int tag)
{ fprintf(stderr, "  [snd %d vol %d pan %d%s]\n", id, vol, pan, loop ? " loop" : ""); (void)pitch; (void)tag; }
void plat_sound_stop_tag(int tag) { (void)tag; }
void plat_sound_stop_all(void) {}
int plat_sound_active(int id) { (void)id; return 0; }

static uint32_t btn(const char *n)
{
    static const struct { const char *n; uint32_t b; } T[] = {
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"a", BTN_A}, {"b", BTN_B},
        {"c", BTN_C}, {"start", BTN_START}, {"x", BTN_X}, {"y", BTN_Y}, {"z", BTN_Z}, {"mode", BTN_MODE}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(n, T[i].n)) return T[i].b;
    fprintf(stderr, "bad button %s\n", n); exit(2);
}

static void shot(const char *name)
{
    char path[512]; snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *o = fopen(path, "wb");
    fprintf(o, "P6\n320 224\n255\n");
    for (int y = 0; y < 224; y++)
        for (int x = 0; x < 320; x++) {
            uint8_t c;
            if (y < 192 && txt_on[cur ^ 1][y]) c = ((uint8_t *)&fb[cur ^ 1][128 + y][0])[x];
            else if (y < 192) c = (uint8_t)fb[cur ^ 1][y >> 1][x >> 1];
            else c = ((uint8_t *)&fb[cur ^ 1][96][0])[(y - 192) * 320 + x];
            uint16_t v = cram[c];
            unsigned char rgb[3] = { (unsigned char)((v & 31) << 3), (unsigned char)(((v >> 5) & 31) << 3), (unsigned char)(((v >> 10) & 31) << 3) };
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
    fprintf(stderr, "shot %s: pos (%d,%d,%d) ang %d sect %d hp %d lvl %d tic %d kills %d/%d\n", name, pl.posx, pl.posy,
            pl.posz, pl.ang, pl.cursectnum, pl.health, level_index, gametic, pl.actors_killed, pl.max_actors_killed);
}

static uint32_t press_mask; static int press_left;

uint32_t plat_read_input(void)
{
    return pad | (press_left > 0 ? press_mask : 0) | BTN_SIX * 0;
}

void plat_flip_request(void) { plat_flip(); }
void plat_flip_wait(void) {}
void plat_flip(void)
{
    cur ^= 1;
    ticks += 2;          /* pretend each frame takes 2 vblanks (30 fps) */
    if (press_left > 0) press_left--;
    if (frames_to_wait > 0) { frames_to_wait--; return; }
    char line[256];
    while (fgets(line, sizeof line, script)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char cmd[32], a1[32], a2[32]; a1[0] = a2[0] = 0;
        int k = sscanf(line, "%31s %31s %31s", cmd, a1, a2);
        if (k <= 0) continue;
        if (!strcmp(cmd, "wait")) { frames_to_wait = atoi(a1); return; }
        if (!strcmp(cmd, "hold")) { pad |= btn(a1); if (k > 2) pad |= btn(a2); continue; }
        if (!strcmp(cmd, "release")) { pad &= ~btn(a1); if (k > 2) pad &= ~btn(a2); continue; }
        if (!strcmp(cmd, "press")) { press_mask = btn(a1); press_left = k > 2 ? atoi(a2) : 2; frames_to_wait = press_left; return; }
        if (!strcmp(cmd, "shot")) { shot(a1); continue; }
        if (!strcmp(cmd, "dump")) {
            for (int q = 0; q < MAXSPRITES; q++) {
                spritetype *sp = &sprite[q];
                if (sp->statnum >= MAXSTATUS) continue;
                if (klabs(sp->x - pl.posx) + klabs(sp->y - pl.posy) > 4000) continue;
                fprintf(stderr, "  spr %d pic %d stat %d sect %d cstat %x extra %d xy (%d,%d) z %d rep %d/%d act t0 %d def %d\n", q, sp->picnum,
                        sp->statnum, sp->sectnum, sp->cstat, sp->extra, sp->x, sp->y, sp->z, sp->xrepeat, sp->yrepeat, act[q].t[0], act[q].extra2);
            }
            continue; }
        if (!strcmp(cmd, "crane_status")) {
            for (int q = headspritestat[STAT_STANDABLE]; q >= 0; q = nextspritestat[q])
                if (sprite[q].picnum >= CRANE && sprite[q].picnum <= CRANE + 2)
                    fprintf(stderr, "crane %d hitag %d pic %d state %d owner %d sect %d xy %d,%d z %d target %d,%d sect %d\n", q, sprite[q].hitag, sprite[q].picnum, act[q].t3, sprite[q].owner, sprite[q].sectnum, sprite[q].x, sprite[q].y, sprite[q].z, act[act[q].t[2]].t[0], act[act[q].t[2]].t[1], act[act[q].t[2]].t[2]);
            continue; }
        if (!strcmp(cmd, "turret_damage")) {
            int q; for (q = headspritestat[STAT_ACTOR]; q >= 0; q = nextspritestat[q]) if (sprite[q].picnum == ROTATEGUN) break;
            if (q >= 0) { actor_damage(q, atoi(a1), atoi(a2) ? RPG : SHOTSPARK1, pl.i); fprintf(stderr, "turret_damage id %d stat %d hp %d kills %d\n", q, sprite[q].statnum, sprite[q].extra, pl.actors_killed); }
            continue; }
        if (!strcmp(cmd, "test_recon")) { /* stage an existing flying Pig Cop for aim tests */
            int q;
            for (q = headspritestat[STAT_ACTOR]; q >= 0; q = nextspritestat[q])
                if (sprite[q].picnum == RECON && sprite[q].extra > 0) break;
            if (q < 0) { fprintf(stderr, "test_recon: no live RECON\n"); continue; }
            char a3[32] = "0";
            sscanf(line, "%*s %*s %*s %31s", a3);
            staged_recon = q;
            sprite[q].x = pl.posx + atoi(a1); sprite[q].y = pl.posy + atoi(a2);
            sprite[q].z = pl.posz + atoi(a3);
            changespritesect((int16_t)q, pl.cursectnum);
            pl.ang = getangle(sprite[q].x - pl.posx, sprite[q].y - pl.posy);
            fprintf(stderr, "test_recon staged: id %d hp %d z %d playerz %d\n", q, sprite[q].extra, sprite[q].z, pl.posz);
            continue; }
        if (!strcmp(cmd, "recon_damage")) {
            if (staged_recon >= 0) actor_damage(staged_recon, 200, atoi(a1) ? RPG : SHOTSPARK1, pl.i);
            continue; }
        if (!strcmp(cmd, "crash_status")) {
            int crashes = 0, pigs = 0, explosions = 0, scrap = 0;
            for (int q = 0; q < MAXSPRITES; q++) {
                if (sprite[q].statnum == STAT_RECON_CRASH) crashes++;
                if (sprite[q].statnum == STAT_ACTOR && sprite[q].picnum == PIGCOP && sprite[q].extra > 0) pigs++;
                if (sprite[q].statnum == STAT_MISC && sprite[q].picnum == EXPLOSION2) explosions++;
                if (sprite[q].statnum == STAT_MISC && sprite[q].picnum >= SCRAP1 && sprite[q].picnum < SCRAP1 + 4) scrap++;
            }
            fprintf(stderr, "crash_status: crashes %d z %d pigs %d explosions %d scrap %d kills %d max %d\n", crashes,
                    staged_recon >= 0 ? sprite[staged_recon].z : 0, pigs, explosions, scrap, pl.actors_killed, pl.max_actors_killed);
            continue; }
        if (!strcmp(cmd, "recon_status")) {
            int q = staged_recon;
            if (q >= 0) fprintf(stderr, "recon_status: id %d hp %d z %d target %d state %d\n", q, sprite[q].extra,
                               sprite[q].z, aim(&sprite[pl.i], AUTO_AIM_ANGLE, pl.posx, pl.posy), sprite[q].statnum);
            continue; }
        if (!strcmp(cmd, "setangle")) { pl.ang = (int16_t)(atoi(a1) & 2047); continue; }
        if (!strcmp(cmd, "face")) {
            int best = -1; int32_t bd = 1 << 30;
            for (int q = headspritestat[STAT_ACTOR]; q >= 0; q = nextspritestat[q]) {
                int32_t d = klabs(sprite[q].x - pl.posx) + klabs(sprite[q].y - pl.posy);
                if (sprite[q].extra > 0 && d < bd && sprite[q].picnum != CAMERA1) { bd = d; best = q; }
            }
            if (best >= 0) pl.ang = (int16_t)getangle(sprite[best].x - pl.posx, sprite[best].y - pl.posy);
            continue; }
        if (!strcmp(cmd, "tag_probe")) {
            int16_t ns, nw, np; int32_t dist;
            neartag(pl.posx, pl.posy, pl.posz, pl.cursectnum, (pl.ang + atoi(a1)) & 2047, &ns, &nw, &np, &dist, 1280, 1);
            fprintf(stderr, "tag_probe %s: sector %d wall %d sprite %d dist %d\n", a1, ns, nw, np, dist);
            continue; }
        if (!strcmp(cmd, "sector_effector")) {
            int n = atoi(a1);
            for (int q = headspritesect[n]; q >= 0; q = nextspritesect[q])
                if (sprite[q].statnum == STAT_EFFECTOR)
                    fprintf(stderr, "sector_effector %d: id %d kind %d speed %d state %d\n", n, q, sprite[q].lotag, sprite[q].yvel, act[q].t[0]);
            continue; }
        if (!strcmp(cmd, "sprite_status")) {
            int n = atoi(a1);
            fprintf(stderr, "sprite_status %d: pic %d tag %d sect %d xy %d,%d z %d\n", n, sprite[n].picnum, sprite[n].lotag, sprite[n].sectnum, sprite[n].x, sprite[n].y, sprite[n].z);
            continue; }
        if (!strcmp(cmd, "wall_status")) {
            int n = atoi(a1);
            fprintf(stderr, "wall_status %d: pic %d cstat %x over %d\n", n, wall[n].picnum, wall[n].cstat, wall[n].overpicnum);
            continue; }
        if (!strcmp(cmd, "probe")) {
            int a = atoi(a1);
            int16_t hs, hw, hsp; int32_t hx, hy, hz;
            hitscan(pl.posx, pl.posy, pl.posz, pl.cursectnum, sintable[(a + 512) & 2047], sintable[a & 2047], 0,
                    &hs, &hw, &hsp, &hx, &hy, &hz, 0);
            if (hw >= 0) fprintf(stderr, "  probe ang %d: wall %d pic %d over %d cstat %x next %d (lotag %d)\n", a, hw, wall[hw].picnum,
                                 wall[hw].overpicnum, wall[hw].cstat, wall[hw].nextsector, wallx[hw].lotag);
            for (int q = 0; q < MAXSPRITES; q++) {
                spritetype *sp = &sprite[q];
                if (sp->statnum >= MAXSTATUS || (sp->cstat & 32768)) continue;
                int32_t d = klabs(sp->x - hx) + klabs(sp->y - hy);
                if (d < 1200) fprintf(stderr, "    near: spr %d pic %d cstat %x stat %d rep %d/%d z %d d %d\n", q, sp->picnum, sp->cstat, sp->statnum,
                                      sp->xrepeat, sp->yrepeat, sp->z, d);
            }
            continue; }
#ifdef RDEBUG_PIC
        if (!strcmp(cmd, "picat")) {
            extern int16_t rdbg_pic_m[RMAXYDIM][RMAXXDIM], rdbg_pic_s[RMAXYDIM][RMAXXDIM];
            int x = atoi(a1), y = atoi(a2);
            fprintf(stderr, "  pic at (%d,%d): m %x s %x  pix %d\n", x, y, rdbg_pic_m[y][x], rdbg_pic_s[y][x], fb[cur ^ 1][y][x] & 255);
            continue; }
#else
        if (!strcmp(cmd, "picat")) continue;
#endif
        if (!strcmp(cmd, "win")) { pl.level_done = 1; pl.timebeforeexit = 1; continue; }
        if (!strcmp(cmd, "god")) { pl.cheat_god = 1; continue; }
        if (!strcmp(cmd, "trig")) { operatemasterswitches(atoi(a1)); operateactivators(atoi(a1), 0); continue; }
        if (!strcmp(cmd, "count")) {  /* count live sprites of a picnum + total sprites in use */
            int pic = atoi(a1), n = 0, tot = 0, armed = 0;
            for (int q = 0; q < MAXSPRITES; q++) if (sprite[q].statnum < MAXSTATUS) { tot++; if (sprite[q].picnum == pic) { n++; if (sprite[q].shade <= -31) armed++; } }
            fprintf(stderr, "count pic %d: %d (armed %d) total sprites %d quake %d tic %d\n", pic, n, armed, tot, earthquaketime, gametic);
            continue; }
        if (!strcmp(cmd, "weapon")) { pl.curr_weapon = (int16_t)atoi(a1); pl.kickback_pic = 0; continue; }
        if (!strcmp(cmd, "stat")) {   /* one-line state: pos, sector, health, level, kills, done */
            fprintf(stderr, "stat %s: pos (%d,%d,%d) ang %d sect %d hp %d lvl %d kills %d/%d done %d exit %d tic %d weap %d ammo %d horiz %d\n", a1,
                    pl.posx, pl.posy, pl.posz, pl.ang, pl.cursectnum, pl.health, level_index, pl.actors_killed,
                    pl.max_actors_killed, pl.level_done, pl.timebeforeexit, gametic, pl.curr_weapon, pl.ammo[pl.curr_weapon], pl.horiz);
            for (int q = headspritestat[STAT_ACTOR]; q >= 0; q = nextspritestat[q])
                if (sprite[q].picnum >= BOSS1 && sprite[q].picnum <= BOSS3 + 49)
                    fprintf(stderr, "  boss spr %d pic %d pal %d hp %d state %d xy (%d,%d) rep %d clip %d\n", q, sprite[q].picnum,
                            sprite[q].pal, sprite[q].extra, act[q].t[0], sprite[q].x, sprite[q].y, sprite[q].xrepeat, sprite[q].clipdist);
            continue; }
        if (!strcmp(cmd, "operate")) { operatesectors(atoi(a1), pl.i); continue; }
        if (!strcmp(cmd, "effector")) {
            int sn = atoi(a1);
            for (int q = headspritesect[sn]; q >= 0; q = nextspritesect[q])
                if (sprite[q].picnum == SECTOREFFECTOR && sprite[q].lotag == 30)
                    fprintf(stderr, "subway sector %d x %d y %d angle %d velocity %d target %d state %d\n",
                            sn, sprite[q].x, sprite[q].y, sprite[q].ang, sprite[q].xvel, sprite[q].owner, act[q].timer);
            continue;
        }
        if (!strcmp(cmd, "test_player_z")) {
            pl.posz = pl.oposz = atoi(a1); pl.poszv = 0;
            setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
            continue; }
        if (!strcmp(cmd, "test_switch")) {
            int n = atoi(a1);
            if (n >= 0 && n < MAXSPRITES && sprite[n].statnum < MAXSTATUS) checkhitswitch(n, 1);
            continue; }
        if (!strcmp(cmd, "test_remove_actor")) {
            int n = atoi(a1);
            if (n >= 0 && n < MAXSPRITES && sprite[n].statnum == STAT_ACTOR) deletesprite((int16_t)n);
            continue; }
        if (!strcmp(cmd, "sect")) {   /* one sector's planes */
            int n = atoi(a1);
            fprintf(stderr, "sect %d: floorz %d ceilingz %d floorstat %x ceilingstat %x fpic %d cpic %d lotag %d\n", n,
                    sector[n].floorz, sector[n].ceilingz, sector[n].floorstat, sector[n].ceilingstat,
                    sector[n].floorpicnum, sector[n].ceilingpicnum, sector[n].lotag);
            continue; }
        if (!strcmp(cmd, "boom")) {   /* set off the C-9 charges of trigger group N */
            int n = atoi(a1), c = 0;
            for (int q = 0; q < MAXSPRITES; q++)
                if (sprite[q].statnum < MAXSTATUS && (sprite[q].picnum == SEENINE || sprite[q].picnum == OOZFILTER) &&
                    sprite[q].hitag == n) { sprite[q].shade = -31; c++; }
            fprintf(stderr, "boom %d: %d charges\n", n, c);
            continue; }
        if (!strcmp(cmd, "hurt")) { player_hurt(atoi(a1), -1); continue; }
        if (!strcmp(cmd, "setz")) { pl.posz = atoi(a1); pl.oposz = pl.posz; pl.poszv = 0; setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT); continue; }
        if (!strcmp(cmd, "shotgun_status")) {
            fprintf(stderr, "shotgun_status %s: owned %d ammo %d equipped %d level %d\n", a1,
                    !!(pl.gotweapon & (1 << SHOTGUN_WEAPON)), pl.ammo[SHOTGUN_WEAPON], pl.curr_weapon, level_index);
            continue; }
        if (!strcmp(cmd, "scuba_status")) {
            fprintf(stderr, "scuba_status %s: amount %d active %d icon %d level %d hp %d\n", a1,
                    pl.inv[INV_SCUBA], pl.scuba_on, pl.inven_icon, level_index, pl.health);
            continue; }
        if (!strcmp(cmd, "pstat")) {  /* falling / view state */
            fprintf(stderr, "pstat %s: tic %d sect %d z %d zv %d fall %d ground %d hp %d horiz %d off %d hard %d rtc %d warp %d hold %d sway %d bob %d\n", a1,
                    gametic, pl.cursectnum, pl.posz, pl.poszv, pl.falling_counter, pl.on_ground, pl.health, pl.horiz,
                    pl.horizoff, pl.hard_landing, pl.return_to_center, pl.on_warping_sector, pl.transporter_hold,
                    pl.weapon_sway, pl.bobcounter);
            continue; }
        if (!strcmp(cmd, "give")) { pl.gotweapon = 0xffff; for (int w = 0; w < MAX_WEAPONS; w++) pl.ammo[w] = 50; continue; }
        if (!strcmp(cmd, "goto")) { /* teleport for testing: goto x y [ang] */
            char a3[32] = "";
            sscanf(line, "%*s %*s %*s %31s", a3);
            pl.posx = atoi(a1); pl.posy = atoi(a2);
            int16_t sn = pl.cursectnum;
            for (int q = 0; q < numsectors; q++) if (inside(pl.posx, pl.posy, (int16_t)q) == 1) { sn = (int16_t)q; break; }
            pl.cursectnum = sn;
            pl.posz = sector[sn].floorz - (40 << 8); pl.poszv = 0; pl.posxv = pl.posyv = 0;
            if (a3[0]) pl.ang = (int16_t)atoi(a3);
            setsprite(pl.i, pl.posx, pl.posy, pl.posz + PHEIGHT);
            fprintf(stderr, "goto -> sect %d z %d\n", sn, pl.posz);
            continue; }
        fprintf(stderr, "bad script line %s\n", line); exit(2);
    }
    longjmp(done_jmp, 1);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: hostgame blob script outdir\n"); return 1; }
    FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *blob = malloc(n); if (fread(blob, 1, n, f) != (size_t)n) return 1; fclose(f);
    script = fopen(argv[2], "r"); outdir = argv[3];
    if (engine_init(blob)) { fprintf(stderr, "bad blob\n"); return 1; }
    game_skip_intro = getenv("DUKE_INTRO") == NULL;   /* scripts start at the title */
    render_init();
    if (setjmp(done_jmp) == 0) game_main();
    fprintf(stderr, "script done at tic %d\n", gametic);
    return 0;
}
