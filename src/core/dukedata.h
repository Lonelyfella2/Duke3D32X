/*
 * Duke Nukem 3D 32X - ROM data blob layout.
 *
 * Produced by tools/convert.py from the user's DUKE3D.GRP.  The blob is
 * written twice: big-endian (linked into the 32X ROM) and little-endian
 * (loaded by the desktop oracle build), so the structures below are read
 * in place on both targets.  Every offset is relative to the blob start.
 *
 * Keep in sync with tools/layout.py.
 */
#ifndef DUKEDATA_H
#define DUKEDATA_H

#include <stdint.h>

#define DK_MAGIC    0x324B4B44u /* 'DKK2' read as a native u32 */
#define DK_VERSION  3

/* ---------------------------------------------------------------- header */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t blob_size;
    uint32_t num_tiles;        /* tile directory slots (MAXTILES)         */
    uint32_t off_tiledir;      /* dk_tile_t[num_tiles]                    */
    uint32_t off_palette;      /* uint16_t[256] 32X CRAM colours          */
    uint32_t off_palookup;     /* uint32_t[DK_MAXPALS] -> 32*256 tables   */
    uint32_t num_maps;
    uint32_t off_mapdir;       /* dk_map_t[num_maps]                      */
    uint32_t num_sounds;
    uint32_t off_sounddir;     /* dk_sound_t[num_sounds]                  */
    uint32_t off_altpals;      /* uint16_t[DK_NUMALTPALS][256]            */
    uint32_t off_sintable;     /* int16_t[2048] (Build TABLES.DAT)        */
    uint32_t off_music;        /* dk_music_t directory (0 = none)         */
    uint32_t num_music;
    uint32_t off_strings;      /* misc text (quotes)                      */
    uint32_t off_trans;        /* 64K translucency table (0 = dithered)   */
    uint32_t flags;            /* DKF_*                                   */
    uint32_t off_radarang;     /* int16_t[1280] (TABLES.DAT, mirrored)    */
    uint32_t off_font;         /* 8x8 textfont[1024] + 4x6 small[1024]    */
    uint32_t off_pics;         /* dk_pic_t[num_pics]: full-screen pictures */
    uint32_t num_pics;
    uint32_t reserved[10];
} dk_header_t;

/* Pictures from assets/: full screens and the title logo sprites.  Raw
 * 8-bit pixels shown at native resolution; each refers to a 256-colour
 * palette (the logo shares the title's).  Index 255 is see-through. */
typedef struct {
    uint32_t off_px;           /* uint8_t[h][w] palette indices (0 = absent),
                                  or two LZ4 blocks when lz[0] != 0       */
    uint16_t w, h;
    uint32_t off_cram;         /* uint16_t[256] CRAM colours for this picture */
    int16_t  x, y;             /* screen position (sprites; 0,0 = full screen) */
    uint32_t lz[2];            /* LZ4 block sizes: rows 0..191, rows 192..h-1
                                  (full screens shown whole; 0 = raw)     */
} dk_pic_t;                    /* 24 bytes */
#define DK_PIC_TITLE       0   /* assets/title.png: title background      */
#define DK_PIC_TRANSITION  1   /* assets/transition.png: entering / tally */
#define DK_PIC_REALMS      2   /* assets/realms.png: power-on 3D Realms   */
#define DK_PIC_LOGO_DUKE   3   /* assets/title_logo.png, "DUKE NUKEM" part */
#define DK_PIC_LOGO_3D     4   /* assets/title_logo.png, "3D" part        */
#define DK_PIC_LOGO_32X    5   /* assets/title_32x.png: 32X part          */
#define DK_PIC_SEETHROUGH  255 /* transparent pixel value in sprites       */

#define DKF_FULL_GAME   1u     /* built from a registered (full) GRP      */

#define DK_MAXPALS      32
#define DK_NUMSHADES    32
#define DK_NUMALTPALS   5      /* water, slime/nightvision, title, 3dr, ending */
#define ALTPAL_WATER    0
#define ALTPAL_SLIME    1
#define ALTPAL_TITLE    2

/* ------------------------------------------------------------ tile dir  */
typedef struct {
    uint16_t w, h;             /* stored size (after downscale)           */
    uint16_t ow, oh;           /* original Build size                     */
    uint8_t  shift;            /* downscale shift (0 = full, 1 = half)    */
    uint8_t  lw, lh;           /* log2 of pow2-ceil(stored w/h)           */
    uint8_t  flags;            /* DKT_*                                   */
    int8_t   xoff, yoff;       /* picanm offsets (original pixels)        */
    uint8_t  anim;             /* picanm frames | type << 6               */
    uint8_t  speed;            /* picanm speed                            */
    uint32_t off;              /* column-major pixels, then spans         */
} dk_tile_t;                   /* 20 bytes */

#define DKT_PRESENT  1         /* tile has pixel data in the ROM          */
#define DKT_TRANS    2         /* contains index 255 (transparent)        */
#define DKT_SPANS    4         /* u8 top[w], u8 bot[w] follow the pixels  */
#define DKT_POW2H    8         /* stored height is a power of two         */

/* -------------------------------------------------------------- maps    */
typedef struct {
    char     name[8];          /* "E1L1"                                  */
    char     title[32];        /* "HOLLYWOOD HOLOCAUST"                   */
    uint32_t off_hdr;          /* dk_maphdr_t (uncompressed)              */
    uint32_t off_lz[3];        /* LZ4 blocks: sectors, walls, sprites     */
    uint32_t lz_size[3];
    uint32_t off_wallx;        /* dk_wallx_t[numwalls], uncompressed ROM  */
    uint16_t music;            /* index into music dir (0xffff = none)    */
    uint16_t par_time;         /* seconds                                 */
    uint16_t episode, level;   /* original numbering (for the HUD)        */
} dk_map_t;                    /* 80 bytes */

typedef struct {
    int32_t  posx, posy, posz;
    int16_t  ang, cursectnum;
    uint16_t numsectors, numwalls, numsprites, pad;
} dk_maphdr_t;                 /* 24 bytes */

/* Runtime map structures - decompressed straight into SDRAM. */
typedef struct {
    int32_t  ceilingz, floorz;
    int16_t  wallptr, wallnum;
    uint16_t ceilingstat, floorstat;
    int16_t  ceilingpicnum, floorpicnum;
    int16_t  ceilingheinum, floorheinum;
    int8_t   ceilingshade, floorshade;
    uint8_t  ceilingpal, floorpal;
    uint8_t  ceilingxpanning, ceilingypanning;
    uint8_t  floorxpanning, floorypanning;
    int16_t  lotag, hitag;
    uint8_t  visibility, filler;
    int16_t  extra;
} sectortype;                  /* 40 bytes */

typedef struct {
    int32_t  x, y;
    int16_t  point2, nextsector;
    uint16_t cstat;
    int16_t  picnum, overpicnum;
    int8_t   shade;
    uint8_t  pal;
    uint8_t  xrepeat, yrepeat;
    uint8_t  xpanning, ypanning;
} walltype;                    /* 24 bytes */

typedef struct {               /* static wall data kept in ROM            */
    int16_t  nextwall, lotag, hitag, extra;
} dk_wallx_t;                  /* 8 bytes */

typedef struct {
    int32_t  x, y, z;
    uint16_t cstat;
    int16_t  picnum;
    int8_t   shade;
    uint8_t  pal;
    uint8_t  clipdist, filler;
    uint8_t  xrepeat, yrepeat;
    int8_t   xoffset, yoffset;
    int16_t  sectnum, statnum;
    int16_t  ang, owner;
    int16_t  xvel, yvel, zvel;
    int16_t  lotag, hitag, extra;
} spritetype;                  /* 44 bytes */

/* -------------------------------------------------------------- sounds  */
typedef struct {
    uint32_t off;              /* IMA ADPCM nibbles (or PCM if bits == 8) */
    uint32_t samples;          /* number of decoded samples               */
    uint16_t rate;             /* Hz                                      */
    uint8_t  format;           /* 0 = 8-bit signed PCM, 1 = IMA ADPCM     */
    uint8_t  priority;
    int8_t   pitch_lo, pitch_hi; /* CON pitch variation                   */
    uint8_t  flags;            /* CON sound type bits (loop=1, ...)       */
    uint8_t  volume;
} dk_sound_t;                  /* 16 bytes */

#define SNDF_LOOP     1
#define SNDF_MSFX     2
#define SNDF_TALK     4
#define SNDF_GLOBAL   16

/* -------------------------------------------------------------- music   */
typedef struct {
    char     name[12];
    uint32_t off;              /* event stream for the 68000 FM player    */
    uint32_t size;
} dk_music_t;

#endif
