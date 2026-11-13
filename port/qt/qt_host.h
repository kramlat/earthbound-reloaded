#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Hand the process arguments to Qt (main.c calls this before anything is initialised). */
void qt_host_set_args(int argc, char **argv);
/* Run Qt's event loop for what is pending: key, mouse and window events become SDL events for
 * the shared input code, and the picture is drawn. Called once per frame. */
void qt_host_pump(void);
#ifdef __cplusplus
}
#endif
