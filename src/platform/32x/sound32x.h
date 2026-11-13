#ifndef SOUND32X_H
#define SOUND32X_H
#include <stdint.h>
void snd_slave_init(void);
void snd_play(int id, int vol, int pan, int32_t pitch, int loop, int tag);
void snd_stop_tag(int tag);
void snd_stop_all(void);
int  snd_active(int id);
#endif
