/*
 * Unix/SDL2 save data implementation — file-backed persistent storage.
 *
 * Save data is stored in a single file (default "earthbound.srm").
 * The file path can be overridden via --save on the command line.
 *
 * Writes are crash-safe: platform_save_write() reads the current file,
 * applies the requested change to an in-memory copy, writes the full
 * result to a temp file next to it, flushes it to stable storage, and
 * only then renames it over the live file. A crash or power loss at any
 * point leaves either the old file or the new one intact, never a
 * half-written one -- the same approach sdl2_savestate.c already uses
 * for its ping-pong slots.
 */
#include "platform/platform.h"
#include "include/constants.h"   /* EB_SAVE_PER_SLOT_IMAGE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>        /* _commit, _fileno */
#else
#include <unistd.h>    /* fsync, fileno, close */
#include <fcntl.h>     /* open, O_RDONLY */
#endif

/* With more than the retail 3 slots every slot is its own 8 KB SRAM image, one file each:
 * save001.srm, save002.srm, ... next to the directory of the --save path (default: the
 * working directory). --save FILE names image 0. A single-image build uses FILE alone. */
static const char *save_file_path = NULL;        /* --save override, or NULL */
static char cur_path[1024] = "earthbound.srm";   /* file the current read/write touches */

void platform_save_set_path(const char *path) {
    save_file_path = path;
}

/* Pick the file for `offset`; returns the offset within it. */
static size_t select_file(size_t offset) {
#ifdef EB_SAVE_PER_SLOT_IMAGE
    size_t image = offset / PLATFORM_SAVE_IMAGE_SIZE;
    if (image == 0 && save_file_path) {
        snprintf(cur_path, sizeof(cur_path), "%s", save_file_path);
    } else {
        char dir[1024];
        if (save_file_path) {                      /* --save FILE: its neighbours sit beside it */
            snprintf(dir, sizeof(dir), "%s", save_file_path);
            char *slash = strrchr(dir, '/');
            if (slash) slash[1] = '\0'; else dir[0] = '\0';
        } else {
            snprintf(dir, sizeof(dir), "%s/", platform_data_dir());
        }
        snprintf(cur_path, sizeof(cur_path), "%ssave%03d.srm", dir, (int)image + 1);
    }
    return offset % PLATFORM_SAVE_IMAGE_SIZE;
#else
    if (save_file_path) snprintf(cur_path, sizeof(cur_path), "%s", save_file_path);
    else snprintf(cur_path, sizeof(cur_path), "%s/earthbound.srm", platform_data_dir());
    return offset;
#endif
}

bool platform_save_init(void) {
    /* Nothing to initialize for file-backed saves */
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    offset = select_file(offset);
    FILE *f = fopen(cur_path, "rb");
    if (!f) return 0;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    size_t read = fread(dst, 1, size, f);
    fclose(f);
    return read;
}

/* "<save_file_path>.tmp" -- staged next to the real file so the final
 * rename() is same-directory (required for it to be atomic). */
static bool make_tmp_path(char *out, size_t out_size) {
    int n = snprintf(out, out_size, "%s.tmp", cur_path);
    return n > 0 && (size_t)n < out_size;
}

#ifndef _WIN32
/* Best-effort: fsync the directory holding save_file_path so the rename
 * itself is durable, not just the file contents. A failure here doesn't
 * undo the write -- the rename already landed on disk by this point,
 * this only protects the directory entry against a second, immediately
 * following crash. */
static void fsync_containing_dir(void) {
    char dir[1024];
    strncpy(dir, cur_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    const char *dir_path = ".";
    if (slash) {
        *slash = '\0';
        if (dir[0] != '\0') dir_path = dir;
    }

    int fd = open(dir_path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}
#endif

bool platform_save_write(const void *src, size_t offset, size_t size) {
    offset = select_file(offset);
    /* Read whatever's on disk now so a write that only touches one
     * SaveBlock copy (or the 2-byte version word) doesn't lose the rest
     * of the file -- callers always write a sub-range, never the whole
     * image in one call. */
    size_t existing = 0;
    FILE *rf = fopen(cur_path, "rb");
    if (rf) {
        if (fseek(rf, 0, SEEK_END) == 0) {
            long len = ftell(rf);
            if (len > 0) existing = (size_t)len;
        }
        fseek(rf, 0, SEEK_SET);
    }

    size_t buf_size = existing > offset + size ? existing : offset + size;
    unsigned char *buf = calloc(1, buf_size);
    if (!buf) {
        if (rf) fclose(rf);
        return false;
    }
    if (rf) {
        fread(buf, 1, existing, rf);
        fclose(rf);
    }
    memcpy(buf + offset, src, size);

    char tmp_path[1024];
    if (!make_tmp_path(tmp_path, sizeof(tmp_path))) {
        free(buf);
        return false;
    }

    FILE *wf = fopen(tmp_path, "wb");
    if (!wf) {
        free(buf);
        return false;
    }
    size_t written = fwrite(buf, 1, buf_size, wf);
    free(buf);

    bool ok = (written == buf_size) && (fflush(wf) == 0);
#ifdef _WIN32
    if (ok && _commit(_fileno(wf)) != 0) /* durability: the new file must hit stable storage */
        ok = false;
#else
    if (ok && fsync(fileno(wf)) != 0) /* durability: the new file must hit stable storage */
        ok = false;
#endif
    if (fclose(wf) != 0)
        ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

#ifdef _WIN32
    /* POSIX rename() atomically replaces an existing destination; the
     * MSVC/mingw C runtime's rename() does not, so the old file has to be
     * removed first. A crash between remove() and rename() would lose the
     * previous save on Windows specifically -- still strictly better than
     * the old in-place write, which could tear mid-write on every
     * platform (Windows included), corrupting the live save outright
     * rather than, in the worst case, losing one generation of it. */
    remove(cur_path);
#endif
    if (rename(tmp_path, cur_path) != 0) {
        remove(tmp_path);
        return false;
    }

#ifndef _WIN32
    fsync_containing_dir();
#endif
    return true;
}
