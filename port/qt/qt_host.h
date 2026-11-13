#pragma once
#include <SDL.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Hand the process arguments to Qt (main.c calls this before anything is initialised). */
void qt_host_set_args(int argc, char **argv);
/* Run Qt's event loop for what is pending: key, mouse and window events become SDL events for
 * the shared input code, and the picture is drawn. Called once per frame. */
void qt_host_pump(void);
/* The next event Qt produced, in SDL's form (false when none): read before SDL's own queue. */
bool qt_host_poll_event(SDL_Event *out);
#ifdef __cplusplus
}
#endif
