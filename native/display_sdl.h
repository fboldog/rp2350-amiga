#pragma once
#include <stdint.h>

void sdl_display_open(int w, int h);
void sdl_display_push(const uint32_t *argb32);
int  sdl_display_poll(void);   /* returns 1 if user closed the window */
void sdl_display_close(void);
