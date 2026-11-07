#ifndef SDL2_TOUCH_H
#define SDL2_TOUCH_H

#include "platform/platform.h"
#include <SDL.h>
#include <stdbool.h>

/*
 * Touch controls (SDL port; also the base for a later SDL-based Android build).
 *
 * Fingers drive an on-screen d-pad and a few buttons, drawn over the picture while touch is in
 * use, and a tap anywhere else is a mouse click at that spot. SDL's own touch-to-mouse events are
 * switched off so a finger is never both. Coordinates are picture pixels.
 */

void touch_init(void);
bool touch_handle_event(const SDL_Event *e);        /* SDL_FINGER*; true if it was one */
void touch_finger(int id, int px, int py, int phase); /* phase: 0 down, 1 move, 2 up (scripts use this) */
uint16_t touch_pad(void);                            /* PAD_* bits held by the on-screen controls */
void touch_not_in_use(void);                         /* keyboard, mouse or pad took over: hide the controls */
void touch_draw(pixel_t *fb, int pitch_bytes);       /* the overlay, into the frame being presented */

/* Picture pixel under a finger given as window-normalised coordinates (sdl2_video.c). */
bool platform_video_finger_to_picture(float nx, float ny, int *px, int *py);

#endif
