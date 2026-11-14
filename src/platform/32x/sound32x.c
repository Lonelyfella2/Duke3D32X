/*
 * sound32x.c - PWM sound mixer on the slave SH-2 (real-hardware method).
 *
 *  * DMA channel 1 of the slave streams 22050 Hz stereo frames (one long =
 *    L<<16|R) from SDRAM into the PWM L/R pair at 0x20004034; three
 *    256-frame buffers rotate.  CHCR1 = 0x18E5, VCR1 = 66, IPRA DMA level 4,
 *    PWM CTRL = 0x0185 (Chilly Willy / roq32x recipe, proven on hardware).
 *  * Mixing happens in the DMA-end interrupt: start the next (already mixed)
 *    buffer, then mix the one after - per-sample IRQ feeding is silent on
 *    real 32X hardware, so we never do that.
 *  * Voices decode IMA ADPCM from ROM at their native rate (5-11 kHz),
 *    stepped in 16.16 to 22050 Hz with Duke's pitch variation, panned and
 *    mixed with a soft knee above +-384 around the PWM centre.
 *  * The master queues commands in a cache-through ring buffer; the slave
 *    reports which sound ids are playing the same way.
 *  * The mixer never touches the SH-2 divide unit (the renderer owns it).
 */
#include <stdint.h>
#include "32x.h"
#include "sound32x.h"
#include "engine.h"

#define MIX_RATE     22050
#define MIX_FRAMES   256
#define NUM_BUFS     3
#define NUM_VOICES   8
#define CMD_SLOTS    32

typedef struct {
    const uint8_t *data;       /* ADPCM nibbles                         */
    uint32_t pos;              /* 16.16 position in source samples      */
    uint32_t step;             /* 16.16 source samples per output frame */
    uint32_t len;              /* source samples                        */
    uint32_t decoded;          /* index of next sample to decode        */
    int32_t pred, index;       /* ADPCM state                           */
    int32_t cur;               /* last decoded sample                   */
    int32_t voll, volr;        /* 0..256                                */
    int16_t id, tag;
    uint8_t active, loop;
    int32_t start_pred, start_index;
} voice_t;

typedef struct {
    int16_t op, id, vol, pan, loop, tag;
    int32_t pitch;
} sndcmd_t;

enum { OP_PLAY = 1, OP_STOPTAG = 2, OP_STOPALL = 3 };

/* shared master<->slave (always accessed cache-through) */
typedef struct {
    volatile uint32_t head;            /* written by master */
    volatile uint32_t tail;            /* written by slave  */
    sndcmd_t cmd[CMD_SLOTS];
    volatile int16_t playing[NUM_VOICES];
    volatile uint32_t mixed;           /* buffers mixed (telemetry) */
    volatile uint32_t underruns;
} sndshared_t;
static sndshared_t sndshared __attribute__((aligned(16)));
#define SH ((volatile sndshared_t *)((uintptr_t)&sndshared | 0x20000000))

static voice_t voices[NUM_VOICES];
static uint32_t mixbuf[NUM_BUFS][MIX_FRAMES] __attribute__((aligned(16)));
static int playbuf;
static int16_t center;

static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ima_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* ------------------------------------------------- master side API */
static void push_cmd(int op, int id, int vol, int pan, int32_t pitch, int loop, int tag)
{
    uint32_t h = SH->head;
    if (h - SH->tail >= CMD_SLOTS) return;          /* queue full: drop */
    volatile sndcmd_t *c = &SH->cmd[h % CMD_SLOTS];
    c->op = (int16_t)op; c->id = (int16_t)id; c->vol = (int16_t)vol; c->pan = (int16_t)pan;
    c->loop = (int16_t)loop; c->tag = (int16_t)tag; c->pitch = pitch;
    SH->head = h + 1;
}

void snd_play(int id, int vol, int pan, int32_t pitch, int loop, int tag) { push_cmd(OP_PLAY, id, vol, pan, pitch, loop, tag); }
void snd_stop_tag(int tag) { push_cmd(OP_STOPTAG, 0, 0, 0, 0, 0, tag); }
void snd_stop_all(void) { push_cmd(OP_STOPALL, 0, 0, 0, 0, 0, 0); }
int snd_active(int id)
{
    for (int v = 0; v < NUM_VOICES; v++) if (SH->playing[v] == id) return 1;
    return 0;
}

/* ------------------------------------------------- slave side mixer */
static void start_voice(const sndcmd_t *c)
{
    const dk_sound_t *s;
    if (c->id < 0 || c->id >= (int)dk_hdr->num_sounds) return;
    s = &((const dk_sound_t *)DKPTR(dk_hdr->off_sounddir))[c->id];
    if (!s->samples || s->format != 1) return;
    /* pick a free voice, else the oldest non-looping one */
    int v, best = -1;
    for (v = 0; v < NUM_VOICES; v++) if (!voices[v].active) { best = v; break; }
    if (best < 0) {
        uint32_t most = 0;
        for (v = 0; v < NUM_VOICES; v++)
            if (!voices[v].loop && voices[v].decoded >= most) { most = voices[v].decoded; best = v; }
        if (best < 0) return;
    }
    voice_t *vo = &voices[best];
    const uint8_t *d = (const uint8_t *)DKPTR(s->off);
    vo->start_pred = (int16_t)((d[0] << 8) | d[1]);
    vo->start_index = d[2] > 88 ? 88 : d[2];
    vo->data = d + 4;
    vo->len = s->samples;
    /* step = rate/22050 in 16.16 without the divide unit: (rate*194788)>>16 */
    uint32_t base = ((uint32_t)s->rate * 194788u) >> 16;
    vo->step = (uint32_t)(((uint64_t)base * (uint32_t)(c->pitch > 0 ? c->pitch : 65536)) >> 16);
    vo->pos = 0x10000u; vo->decoded = 0;
    vo->pred = vo->start_pred; vo->index = vo->start_index; vo->cur = vo->pred;
    int vol = c->vol, pan = c->pan;
    if (vol > 255) vol = 255;
    if (vol < 0) vol = 0;
    vo->voll = (vol * (128 - (pan > 0 ? pan : 0))) >> 7;
    vo->volr = (vol * (128 + (pan < 0 ? pan : 0))) >> 7;
    vo->id = c->id; vo->tag = c->tag; vo->loop = (uint8_t)c->loop;
    vo->active = 1;
}

static __attribute__((noinline)) void process_cmds(void)
{
    uint32_t t = SH->tail, h = SH->head;
    while (t != h) {
        sndcmd_t c;
        volatile sndcmd_t *src = &SH->cmd[t % CMD_SLOTS];
        c.op = src->op; c.id = src->id; c.vol = src->vol; c.pan = src->pan;
        c.loop = src->loop; c.tag = src->tag; c.pitch = src->pitch;
        switch (c.op) {
        case OP_PLAY: start_voice(&c); break;
        case OP_STOPTAG:
            for (int v = 0; v < NUM_VOICES; v++) if (voices[v].active && voices[v].tag == c.tag) voices[v].active = 0;
            break;
        case OP_STOPALL:
            for (int v = 0; v < NUM_VOICES; v++) voices[v].active = 0;
            break;
        }
        t++;
    }
    SH->tail = t;
}

static inline int32_t decode_step(const uint8_t *data, uint32_t dec, int32_t *predp, int32_t *idxp)
{
    uint32_t b = data[dec >> 1];
    uint32_t code = (dec & 1) ? (b & 15) : (b >> 4);
    int32_t idx = *idxp;
    int32_t st = ima_step[idx];
    int32_t delta = st >> 3;
    if (code & 4) delta += st;
    if (code & 2) delta += st >> 1;
    if (code & 1) delta += st >> 2;
    int32_t pred = *predp + ((code & 8) ? -delta : delta);
    if (pred > 32767) pred = 32767;
    else if (pred < -32768) pred = -32768;
    idx += ima_index[code & 7];
    if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
    *predp = pred; *idxp = idx;
    return pred;
}

static __attribute__((noinline)) void mix_voice_first(voice_t *vo, int32_t *acc)
{
    int32_t sL = vo->cur * vo->voll, sR = vo->cur * vo->volr;
    uint32_t frac = vo->pos, step = vo->step;
    int32_t *p = acc;
    int n = MIX_FRAMES;
    do {
        if (frac >= 0x10000u) {
            uint32_t dec = vo->decoded, len = vo->len;
            const uint8_t *data = vo->data;
            int32_t pred = vo->pred, idx = vo->index;
            do {
                frac -= 0x10000u;
                if (dec >= len) {
                    if (vo->loop) { dec = 0; pred = vo->start_pred; idx = vo->start_index; }
                    else {
                        vo->active = 0;
                        do { p[0] = 0; p[1] = 0; p += 2; } while (--n);
                        goto done;
                    }
                }
                pred = decode_step(data, dec++, &pred, &idx);
            } while (frac >= 0x10000u);
            vo->decoded = dec; vo->pred = pred; vo->index = idx; vo->cur = pred;
            sL = pred * vo->voll; sR = pred * vo->volr;
        }
        p[0] = sL; p[1] = sR; p += 2;
        frac += step;
    } while (--n);
done:
    vo->pos = frac;
}

static __attribute__((noinline)) void mix_voice_add(voice_t *vo, int32_t *acc)
{
    int32_t sL = vo->cur * vo->voll, sR = vo->cur * vo->volr;
    uint32_t frac = vo->pos, step = vo->step;
    int32_t *p = acc;
    int n = MIX_FRAMES;
    do {
        if (frac >= 0x10000u) {
            uint32_t dec = vo->decoded, len = vo->len;
            const uint8_t *data = vo->data;
            int32_t pred = vo->pred, idx = vo->index;
            do {
                frac -= 0x10000u;
                if (dec >= len) {
                    if (vo->loop) { dec = 0; pred = vo->start_pred; idx = vo->start_index; }
                    else { vo->active = 0; goto done; }
                }
                pred = decode_step(data, dec++, &pred, &idx);
            } while (frac >= 0x10000u);
            vo->decoded = dec; vo->pred = pred; vo->index = idx; vo->cur = pred;
            sL = pred * vo->voll; sR = pred * vo->volr;
        }
        p[0] += sL; p[1] += sR; p += 2;
        frac += step;
    } while (--n);
done:
    vo->pos = frac;
}

static __attribute__((noinline)) void mix_buffer(uint32_t *out)
{
    static int32_t acc[MIX_FRAMES * 2];
    int v, any = 0;
    for (v = 0; v < NUM_VOICES; v++) {
        voice_t *vo = &voices[v];
        if (!vo->active) continue;
        if (!any) { mix_voice_first(vo, acc); any = 1; }
        else mix_voice_add(vo, acc);
    }
    if (!any) {
        uint32_t sil = ((uint32_t)center << 16) | (uint16_t)center;
        volatile uint32_t *vout = out;
        int n = MIX_FRAMES;
        do { *vout++ = sil; } while (--n);
    } else {
        const int32_t *p = acc;
        int32_t ctr = center;
        volatile uint32_t *vout = out;
        int n = MIX_FRAMES;
        do {
            int32_t l = p[0] >> 15, r = p[1] >> 15;
            p += 2;
            if (l > 384) l = 384 + ((l - 384) >> 2); else if (l < -384) l = -384 + ((l + 384) >> 2);
            if (r > 384) r = 384 + ((r - 384) >> 2); else if (r < -384) r = -384 + ((r + 384) >> 2);
            if (l > 500) l = 500; else if (l < -500) l = -500;
            if (r > 500) r = 500; else if (r < -500) r = -500;
            *vout++ = ((uint32_t)(ctr + l) << 16) | (uint32_t)(uint16_t)(ctr + r);
        } while (--n);
    }
    for (v = 0; v < NUM_VOICES; v++) SH->playing[v] = voices[v].active ? voices[v].id : -1;
    SH->mixed++;
}

static inline uint32_t *mixbuf_uncached(int idx)
{
    return (uint32_t *)((uintptr_t)mixbuf[idx] | 0x20000000u);
}

static void dma_start(const uint32_t *buf)
{
    SH2_DMA_SAR1 = (uintptr_t)buf;
    SH2_DMA_DAR1 = 0x20004034;            /* PWM L/R pair: one long per stereo frame */
    SH2_DMA_TCR1 = MIX_FRAMES;
    SH2_DMA_CHCR1 = 0x18e5;               /* dest fixed, src incr, long, ext req, dack, intr, enable */
}

void sec_dma1_handler(void)
{
    SH2_DMA_CHCR1;                        /* read TE */
    SH2_DMA_CHCR1 = 0;                    /* clear TE */
    int next = (playbuf + 1) % NUM_BUFS;
    dma_start(mixbuf[next]);              /* keep the PWM fed first */
    int fill = (playbuf + NUM_BUFS - 1) % NUM_BUFS;   /* the buffer that just finished */
    playbuf = next;
    process_cmds();
    mix_buffer(mixbuf_uncached(fill));
}

void snd_slave_init(void)
{
    uint32_t cycle;
    for (int v = 0; v < NUM_VOICES; v++) { voices[v].active = 0; SH->playing[v] = -1; }
    SH->head = SH->tail = 0;

    SH2_DMA_SAR0 = 0; SH2_DMA_DAR0 = 0; SH2_DMA_TCR0 = 0; SH2_DMA_CHCR0 = 0; SH2_DMA_DRCR0 = 0;
    SH2_DMA_SAR1 = 0; SH2_DMA_DAR1 = 0x20004034; SH2_DMA_TCR1 = 0; SH2_DMA_CHCR1 = 0; SH2_DMA_DRCR1 = 0;
    SH2_DMA_DMAOR = 1;                    /* enable DMA */
    SH2_DMA_VCR1 = 66;                    /* vector for DMA channel 1 */
    SH2_INT_IPRA = (SH2_INT_IPRA & 0xF0FF) | 0x0400;     /* DMA IRQ priority 4 */

    if (MARS_VDP_DISPMODE & MARS_NTSC_FORMAT)
        cycle = (((23011361u << 1) / MIX_RATE + 1) >> 1) + 1;
    else
        cycle = (((22801467u << 1) / MIX_RATE + 1) >> 1) + 1;
    center = (int16_t)(cycle >> 1);
    MARS_PWM_CYCLE = (uint16_t)cycle;
    MARS_PWM_CTRL = 0x0185;               /* TM=1 (DREQ), L->left, R->right */

    /* ramp from 2 up to centre to avoid a click on real hardware */
    for (int s = 2; s < center; s++) {
        for (int k = 0; k < 4; k++) {
            while (MARS_PWM_MONO & 0x8000) {}       /* FIFO full */
            MARS_PWM_MONO = (uint16_t)s;
        }
    }
    for (int b = 0; b < NUM_BUFS; b++) {
        volatile uint32_t *dst = mixbuf_uncached(b);
        uint32_t sil = ((uint32_t)center << 16) | (uint16_t)center;
        for (int f = 0; f < MIX_FRAMES; f++) dst[f] = sil;
    }
    playbuf = 0;
    dma_start(mixbuf[0]);
}
