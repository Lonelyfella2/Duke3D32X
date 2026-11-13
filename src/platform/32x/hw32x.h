#ifndef HW32X_H
#define HW32X_H

#include <stdint.h>
#include "32x.h"

#define VIEW_W      160          /* internal 3D view columns (each = 2 px)   */
#define VIEW_H      96           /* internal 3D view rows (each = 2 lines)   */
#define HUD_LINES   32           /* status bar lines at native resolution    */
#define FB_PITCH16  160          /* uint16 words per framebuffer line (320B) */
#define FB_VIEW_OFS 0x100        /* word offset of first view row            */
#define FB_HUD_OFS  (0x100 + VIEW_H * FB_PITCH16)

#define FB16        ((volatile uint16_t *)0x24000000)

/* pad bits (Chilly Willy 68000 pad reader, active high) */
#define PAD_UP      SEGA_CTRL_UP
#define PAD_DOWN    SEGA_CTRL_DOWN
#define PAD_LEFT    SEGA_CTRL_LEFT
#define PAD_RIGHT   SEGA_CTRL_RIGHT
#define PAD_A       SEGA_CTRL_A
#define PAD_B       SEGA_CTRL_B
#define PAD_C       SEGA_CTRL_C
#define PAD_START   SEGA_CTRL_START
#define PAD_X       SEGA_CTRL_X
#define PAD_Y       SEGA_CTRL_Y
#define PAD_Z       SEGA_CTRL_Z
#define PAD_MODE    SEGA_CTRL_MODE

void hw_init(void);
void hw_set_palette(const uint16_t *cram, int first, int count);
void hw_wait_vblank(void);
void hw_flip(void);                 /* show the drawn buffer, wait until swapped */
uint32_t hw_ticks(void);            /* vblank count                             */
uint16_t hw_read_pad(int port);
int hw_is_pal(void);

extern volatile uint32_t mars_vblank_count;

#endif
