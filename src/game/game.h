/*
 * game.h - Duke Nukem 3D 32X gameplay layer (portable C) and the platform
 * services it needs (implemented in src/platform/32x or src/platform/host).
 */
#ifndef GAME_H
#define GAME_H

#include "render.h"

/* ---- platform services ---- */
void      plat_render_view(void);           /* renders rview into the view area */
pixel_t  *plat_hud_base(void);              /* 320x32 native-res HUD (pitch 160 words) */
pixel_t  *plat_view_base(void);
uint32_t  plat_ticks(void);                 /* 60 Hz (50 Hz PAL) vblank counter */
int       plat_pal(void);
uint32_t  plat_read_input(void);            /* pad bits, SEGA_CTRL layout */
void      plat_set_palette(const uint16_t *cram);
void      plat_flip(void);
void      plat_flip_request(void);
void      plat_flip_wait(void);
void      plat_telemetry(uint16_t a, uint16_t b);
/* sound: id = Duke sound number; vol 0..255; pan -128..127; pitch 16.16 rate scale */
void      plat_sound_play(int id, int vol, int pan, int32_t pitch, int loop, int tag);
void      plat_sound_stop_tag(int tag);
void      plat_sound_stop_all(void);
int       plat_sound_active(int id);

/* pad bits (same as the 68000 reader) */
#define BTN_UP     0x0001
#define BTN_DOWN   0x0002
#define BTN_LEFT   0x0004
#define BTN_RIGHT  0x0008
#define BTN_B      0x0010
#define BTN_C      0x0020
#define BTN_A      0x0040
#define BTN_START  0x0080
#define BTN_Z      0x0100
#define BTN_Y      0x0200
#define BTN_X      0x0400
#define BTN_MODE   0x0800
#define BTN_SIX    0x1000

void game_main(void);
extern int game_skip_intro;                 /* 1 = no 3D Realms screen / logo animation */
#define HUD_LINES_G 32
#define SCREEN_VIEW_LINES 192       /* native lines covered by the 3D view   */

/* A writable native 320-byte line of the frame being drawn, for screen line
 * sy (0..223).  Lines over the line-doubled 3D view are copied to a private
 * line the first time they are requested in a frame (the copy holds the
 * view as drawn so far), so 2D text on top is full resolution. */
uint8_t  *plat_text_line(int sy);
/* the same line without the copy of the view row - for pictures that cover
 * the whole line.  Lines 0..191 are consecutive in memory, as are 192..223. */
uint8_t  *plat_screen_line(int sy);
void      plat_frame_begin(void);       /* start drawing a new frame (resets text lines) */

/* hud_text.c - text in the bevelled 8x8 font (assets/font.png) */
enum { TS_YELLOW, TS_BLUE, TS_RED, TS_GRAY, TS_GREEN, TS_COUNT };
#define TXT_H 8
int  txt_width(const char *s);
void txt_draw(int x, int y, const char *s, int style);     /* screen pixels  */
void txt_center(int y, const char *s, int style);
int  txt_center_wrap(int y, const char *s, int style, int maxw);
void txt_fill(int x, int y, int w, int h, int colour);
void txt_palette(const uint16_t *cram);     /* palette on screen (0 = Duke's) */
/* Duke's big red font (assets/dukefont.png): title and pause screens only.
 * cram = the palette on screen; the font's colours are matched to it. */
#define DFONT_HEIGHT 15
int  dfont_width(const char *s);
void dfont_draw(int x, int y, const char *s, const uint16_t *cram);
void dfont_center(int y, const char *s, const uint16_t *cram);
void txt_pic(int x, int y, int pic);        /* 2D tile at native resolution */
/* Tormentstein 3D font (assets/tormentfont.png): top messages, ENTERING /
 * LEVEL COMPLETED, the skill and level menu.  Tints keep the font's shading. */
enum { TF_WHITE, TF_YELLOW, TF_RED, TF_BLUE, TF_COUNT };
#define TFONT_LINE 14                       /* line spacing (12 px glyphs + descender) */
int  tfont_width(const char *s);
void tfont_draw(int x, int y, const char *s, int tint);
void tfont_center(int y, const char *s, int tint);
int  tfont_center_wrap(int y, const char *s, int tint, int maxw);

#endif
