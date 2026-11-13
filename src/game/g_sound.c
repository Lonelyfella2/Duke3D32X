/* g_sound.c - positional sound effects and Duke's one-liners */
#include "g_local.h"

static const dk_sound_t *snddir(int num)
{
    if (num < 0 || num >= (int)dk_hdr->num_sounds) return 0;
    const dk_sound_t *s = &((const dk_sound_t *)DKPTR(dk_hdr->off_sounddir))[num];
    return s->samples ? s : 0;
}

int sound_playing(int num) { return plat_sound_active(num); }

static void play(int num, int32_t x, int32_t y, int positional, int tag)
{
    const dk_sound_t *s = snddir(num);
    if (!s) return;
    int vol = 255, pan = 0;
    if (positional) {
        int32_t dx = x - pl.posx, dy = y - pl.posy;
        int32_t d = (klabs(dx) + klabs(dy));
        d -= d >> 2;
        if (d > 24000) return;
        vol = 255 - (d >> 7);
        if (vol < 24) vol = 24;
        if (d > 256) {
            int a = (getangle(dx, dy) - pl.ang) & 2047;
            pan = (sintable[a] >> 7);              /* right = positive */
        }
    }
    int32_t pitch = 65536;
    if (s->pitch_lo || s->pitch_hi) {
        int lo = s->pitch_lo, hi = s->pitch_hi;
        int r = lo + (int)((uint32_t)krand() % (uint32_t)(hi - lo + 1));
        pitch = 65536 + r * 410;                    /* CON pitch units ~ 1/160 */
    }
    plat_sound_play(num, vol, pan, pitch, (s->flags & SNDF_LOOP) != 0, tag);
}

void spritesound(int num, int spritenum)
{
    if (spritenum < 0 || spritenum == pl.i) { play(num, pl.posx, pl.posy, 0, -1); return; }
    play(num, sprite[spritenum].x, sprite[spritenum].y, 1, spritenum);
}

void globalsound(int num) { play(num, 0, 0, 0, -1); }

void player_talk(int num)
{
    if (pl.talk_timer > 0) return;
    globalsound(num);
    pl.talk_timer = 30 * 4;
}

void sound_stopall(void) { plat_sound_stop_all(); }

/* MUSICANDSFX sprites: ambient loops when the player is in range */
void sound_ambient_update(void)
{
    for (int i = headspritestat[STAT_FX]; i >= 0; i = nextspritestat[i]) {
        spritetype *s = &sprite[i];
        if (s->picnum != MUSICANDSFX || s->lotag >= 1000 || s->lotag <= 0) continue;
        const dk_sound_t *sd = snddir(s->lotag);
        if (!sd || !(sd->flags & SNDF_LOOP)) continue;
        int32_t d = klabs(s->x - pl.posx) + klabs(s->y - pl.posy);
        int32_t range = s->hitag ? s->hitag : 6144;
        if (d < range) {
            if (!act[i].t[0]) { play(s->lotag, s->x, s->y, 1, i); act[i].t[0] = 1; }
        } else if (act[i].t[0]) {
            plat_sound_stop_tag(i);
            act[i].t[0] = 0;
        }
    }
}
