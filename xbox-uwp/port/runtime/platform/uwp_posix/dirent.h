// <dirent.h> for UWP over FindFirstFile/FindNextFile.
//
// Seven files in the runtime list a directory (mods, custom textures, the romfs builder, the
// randomiser, paths.c). MSVC/UCRT has no <dirent.h>, so this provides the three calls those files
// use: opendir, readdir, closedir - and the one field they read, d_name.
//
// Behaviour is deliberately the same as mingw's, because the call sites were written against it:
// "." and ".." are returned, in whatever order the filesystem lists them, and the caller filters
// them out. Anything that changes the set of names would silently change what the game loads.
//
// Not provided: d_type (nothing reads it), telldir/seekdir/rewinddir/scandir, and the _r variants.
// d_name is MAX_PATH bytes: a Win32 file name is at most 255 characters, and callers copy it into
// fixed buffers of 260+.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    char d_name[260];
};

typedef struct {
    HANDLE find;
    WIN32_FIND_DATAA data;
    struct dirent entry;
    int done;  // FindNextFileA has already failed
} DIR;

// Win32 wants '\', the runtime's paths are '/' (they are also used on macOS). Normalised here so
// that listing "mods/sub" works without every caller knowing which platform it is on.
static inline void uwp_dirent_pattern(const char *path, char *out, size_t size) {
    size_t i = 0;
    for (; path[i] && i + 3 < size; i++) out[i] = path[i] == '/' ? '\\' : path[i];
    if (i && out[i - 1] != '\\') out[i++] = '\\';
    if (i + 1 < size) {
        out[i++] = '*';
        out[i] = 0;
    } else if (size) {
        out[size - 1] = 0;
    }
}

static inline DIR *opendir(const char *path) {
    char pattern[1024];
    uwp_dirent_pattern(path ? path : "", pattern, sizeof pattern);
    DIR *d = malloc(sizeof *d);
    if (!d) return NULL;
    d->done = 0;
    d->find = FindFirstFileA(pattern, &d->data);
    if (d->find == INVALID_HANDLE_VALUE) {
        free(d);
        errno = ENOENT;  // the runtime only ever distinguishes "there is nothing to list"
        return NULL;
    }
    return d;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d || d->done) return NULL;
    strncpy(d->entry.d_name, d->data.cFileName, sizeof d->entry.d_name - 1);
    d->entry.d_name[sizeof d->entry.d_name - 1] = 0;
    if (!FindNextFileA(d->find, &d->data)) d->done = 1;  // this entry is still returned
    return &d->entry;
}

static inline int closedir(DIR *d) {
    if (!d) return 0;
    FindClose(d->find);
    free(d);
    return 0;
}

#ifdef __cplusplus
}
#endif
