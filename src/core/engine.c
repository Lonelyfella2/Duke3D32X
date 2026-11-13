/*
 * engine.c - world state, ROM blob access and map loading.
 */
#include "engine.h"
#include "lz4.h"

sectortype sector[MAXSECTORS];
walltype wall[MAXWALLS];
spritetype sprite[MAXSPRITES];
const dk_wallx_t *wallx;
int32_t numsectors, numwalls;

int16_t headspritesect[MAXSECTORS + 1], headspritestat[MAXSTATUS + 1];
int16_t prevspritesect[MAXSPRITES], nextspritesect[MAXSPRITES];
int16_t prevspritestat[MAXSPRITES], nextspritestat[MAXSPRITES];

const uint8_t *dk_blob;
const dk_header_t *dk_hdr;
const dk_tile_t *tiledir;
int32_t numtiles;
const int16_t *sintable;
const int16_t *radarang;
const uint8_t *palookup[DK_MAXPALS];
const uint16_t *dk_cram;
const uint8_t *textfont, *smalltextfont;

volatile int32_t totalclock;
int32_t totalclocklock;
int32_t randomseed = 1;
int16_t editstatus;

int16_t clipnum, hitwalls[4];
int32_t hitscangoalx = (1 << 29) - 1, hitscangoaly = (1 << 29) - 1;
linetype clipit[MAXCLIPNUM];
int16_t clipsectorlist[MAXCLIPNUM], clipsectnum;
int16_t clipobjectval[MAXCLIPNUM];
int32_t rxi[8], ryi[8], rzi[8], rxi2[8], ryi2[8], rzi2[8];
int32_t clipmoveboxtracenum = 3;

const uint8_t pow2char[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
const int32_t pow2long[32] = {
    1L, 2L, 4L, 8L, 16L, 32L, 64L, 128L, 256L, 512L, 1024L, 2048L, 4096L, 8192L,
    16384L, 32768L, 65536L, 131072L, 262144L, 524288L, 1048576L, 2097152L,
    4194304L, 8388608L, 16777216L, 33554432L, 67108864L, 134217728L,
    268435456L, 536870912L, 1073741824L, 2147483647L
};

int32_t ksqrt(uint32_t num)
{
    uint32_t res = 0, bit = 1u << 30;
    while (bit > num) bit >>= 2;
    while (bit) {
        if (num >= res + bit) { num -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return (int32_t)res;
}

int engine_init(const void *blob)
{
    dk_blob = (const uint8_t *)blob;
    dk_hdr = (const dk_header_t *)blob;
    if (dk_hdr->magic != DK_MAGIC || dk_hdr->version != DK_VERSION)
        return -1;
    tiledir = (const dk_tile_t *)DKPTR(dk_hdr->off_tiledir);
    numtiles = (int32_t)dk_hdr->num_tiles;
    sintable = (const int16_t *)DKPTR(dk_hdr->off_sintable);
    radarang = (const int16_t *)DKPTR(dk_hdr->off_radarang);
    dk_cram = (const uint16_t *)DKPTR(dk_hdr->off_palette);
    textfont = (const uint8_t *)DKPTR(dk_hdr->off_font);
    smalltextfont = textfont + 1024;
    const uint32_t *pl = (const uint32_t *)DKPTR(dk_hdr->off_palookup);
    for (int i = 0; i < DK_MAXPALS; i++)
        palookup[i] = (const uint8_t *)DKPTR(pl[i]);
    return 0;
}

int engine_nummaps(void) { return (int)dk_hdr->num_maps; }

const dk_map_t *engine_mapinfo(int mapnum)
{
    if (mapnum < 0 || mapnum >= (int)dk_hdr->num_maps) return 0;
    return &((const dk_map_t *)DKPTR(dk_hdr->off_mapdir))[mapnum];
}

const char *engine_quote(int q)
{
    if (!dk_hdr->off_strings) return "";
    const uint32_t *t = (const uint32_t *)DKPTR(dk_hdr->off_strings);
    if (q < 0 || q >= (int)t[0] || !t[1 + q]) return "";
    return (const char *)DKPTR(t[1 + q]);
}

const dk_pic_t *engine_pic(int n)
{
    if (!dk_hdr->off_pics || n < 0 || n >= (int)dk_hdr->num_pics) return 0;
    const dk_pic_t *p = &((const dk_pic_t *)DKPTR(dk_hdr->off_pics))[n];
    return p->off_px ? p : 0;
}

int engine_loadboard(int mapnum, int32_t *px, int32_t *py, int32_t *pz, int16_t *pang, int16_t *psect)
{
    const dk_map_t *m = engine_mapinfo(mapnum);
    if (!m) return -1;
    const dk_maphdr_t *h = (const dk_maphdr_t *)DKPTR(m->off_hdr);
    if (h->numsectors > MAXSECTORS || h->numwalls > MAXWALLS || h->numsprites > MAXSPRITES)
        return -2;

    initspritelists();
    numsectors = h->numsectors;
    numwalls = h->numwalls;
    lz4_decode(DKPTR(m->off_lz[0]), m->lz_size[0], (uint8_t *)sector, sizeof(sectortype) * numsectors);
    lz4_decode(DKPTR(m->off_lz[1]), m->lz_size[1], (uint8_t *)wall, sizeof(walltype) * numwalls);
    /* sprites are decoded into the tail of the array, then inserted */
    spritetype *tmp = &sprite[MAXSPRITES - h->numsprites];
    lz4_decode(DKPTR(m->off_lz[2]), m->lz_size[2], (uint8_t *)tmp, sizeof(spritetype) * h->numsprites);
    wallx = (const dk_wallx_t *)DKPTR(m->off_wallx);

    /* insertsprite hands out indices 0,1,2... in order, so slot k (< i+1)
     * never overlaps the not-yet-read tail entry tmp[i]. */
    int n = 0;
    for (int i = 0; i < h->numsprites; i++) {
        spritetype s = tmp[i];
        int k = insertsprite(s.sectnum, s.statnum);
        if (k < 0) break;
        sprite[k] = s;
        sprite[k].owner = (int16_t)k;
        n = k + 1;
    }
    /* scrub free slots (the tail still holds decode leftovers) */
    for (int i = n; i < MAXSPRITES; i++) {
        memset(&sprite[i], 0, sizeof(spritetype));
        sprite[i].sectnum = MAXSECTORS;
        sprite[i].statnum = MAXSTATUS;
    }

    *px = h->posx; *py = h->posy; *pz = h->posz;
    *pang = h->ang; *psect = h->cursectnum;
    return 0;
}
