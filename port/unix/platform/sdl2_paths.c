/*
 * Where the game keeps its own files: saves (saveNNN.srm) and settings (input.cfg).
 *
 *   Linux, macOS, other Unix:  ~/.earthbound/
 *   Windows:                   %LOCALAPPDATA%\earthbound\   (AppData\Local\earthbound)
 *
 * EARTHBOUND_HOME overrides the folder (tests, portable installs).
 */
#include "platform/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir((p), 0755)
#endif

const char *platform_data_dir(void) {
    static char dir[1024];
    if (dir[0]) return dir;

    const char *override = getenv("EARTHBOUND_HOME");
    if (override && *override) {
        snprintf(dir, sizeof(dir), "%s", override);
    } else {
#ifdef _WIN32
        const char *local = getenv("LOCALAPPDATA");
        const char *profile = getenv("USERPROFILE");
        if (local && *local) snprintf(dir, sizeof(dir), "%s/earthbound", local);
        else if (profile && *profile) snprintf(dir, sizeof(dir), "%s/AppData/Local/earthbound", profile);
        else snprintf(dir, sizeof(dir), "earthbound");
#else
        const char *home = getenv("HOME");
        if (home && *home) snprintf(dir, sizeof(dir), "%s/.earthbound", home);
        else snprintf(dir, sizeof(dir), ".earthbound");
#endif
    }
    size_t n = strlen(dir);                       /* no trailing separator */
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = '\0';
    MAKE_DIR(dir);                                /* the parent (HOME, LOCALAPPDATA) already exists */
    return dir;
}
