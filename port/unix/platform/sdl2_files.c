/*
 * Unix file and folder access for the in-game save backup / restore browser.
 * Not part of the game proper: the browser lists folders and *.srm files and the
 * backup/restore code copies a save image to or from a path the player picked.
 */
#include "platform/platform.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *platform_home_dir(void) {
    const char *home = getenv("HOME");
    return (home && *home) ? home : ".";
}

bool platform_dir_parent(char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';      /* trailing slashes */
    if (n <= 1) return false;                                   /* already "/" (or empty) */
    char *slash = strrchr(path, '/');
    if (!slash) return false;
    if (slash == path) path[1] = '\0';                          /* "/dir" -> "/" */
    else *slash = '\0';
    return true;
}

static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int entry_cmp(const void *a, const void *b) {
    const PlatformDirEntry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;     /* folders first */
    return strcasecmp(x->name, y->name);
}

int platform_dir_list(const char *dir, const char *ext, PlatformDirEntry *out, int max) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] == '.') continue;                    /* hidden entries and . / .. */
        char full[1024];
        snprintf(full, sizeof(full), "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !(ext && S_ISREG(st.st_mode) && has_ext(de->d_name, ext))) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", de->d_name);
        out[n].is_dir = is_dir;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(out[0]), entry_cmp);
    return n;
}

bool platform_file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

size_t platform_file_read(const char *path, void *dst, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(dst, 1, max, f);
    fclose(f);
    return n;
}

bool platform_file_write(const char *path, const void *data, size_t size) {
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}
