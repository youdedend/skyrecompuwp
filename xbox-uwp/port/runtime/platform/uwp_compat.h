// POSIX-on-UWP compatibility header (included via runtime/platform.h when SKYRECOMP_UWP).
//
// runtime/platform/win_compat.h covers Windows built with mingw-w64 headers (zig cc / clang
// targeting x86_64-windows-gnu), which is what the upstream Makefile does. It leans on
// <unistd.h>, POSIX names for read/write/open, and a mingw `mkdir` signature. None of that
// exists for MSVC/UCRT, which is the only toolchain that can produce an AppContainer image.
//
// So this file supplies the same surface on top of UCRT + <windows.h>. Scope today: everything
// runtime/platform/uwp.c pulls in. Compiling the rest of the runtime will extend it - chiefly
// `<pthread.h>`, which the runtime uses freely (10 pthread_create, 27 pthread_mutex_t) and which
// must come from winpthreads built against the UCRT for UWP, not from this file.
//
// Build with /D_FILE_OFFSET_BITS=64 semantics: off_t here is always 64-bit, because
// romfs.bin alone is over 4 GB.
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---------------------------------------------------------------------------------------
// types
// ---------------------------------------------------------------------------------------
// UCRT's <sys/types.h> defines ssize_t only when the guard is unset; mingw and glibc use
// other names. Set the same guard they do so a later include cannot redefine it.
#ifndef _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#define _SSIZE_T_DEFINED
#endif

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

// ---------------------------------------------------------------------------------------
// POSIX file API names -> UCRT
//
// The runtime calls these by their POSIX names. Under mingw they resolve to the CRT directly;
// here they map onto the underscore-prefixed UCRT entry points. All are legal in an AppContainer
// as long as the path is inside the package, LocalState, or a brokered location.
// ---------------------------------------------------------------------------------------
#ifndef read
#define read  _read
#define write _write
#define close _close
#define open  _open
#define lseek _lseeki64
#define unlink _unlink
#define rmdir  _rmdir
#define getcwd _getcwd
#define chdir  _chdir
#define access _access
#define fstat  _fstat64
#define stat   _stat64
#endif

// <fcntl.h>'s O_* values differ in intent between POSIX and UCRT (O_RDONLY is 0 in both, but
// O_BINARY has no POSIX meaning). The runtime opens files that must be binary on Windows.
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0  // no fork/exec in an AppContainer: nothing to leak the descriptor to
#endif

// ---------------------------------------------------------------------------------------
// positional I/O
//
// UCRT has no pread/pwrite. Doing it with _lseeki64 + read would not be thread-safe, which is
// the entire point of the call (the runtime reads romfs and shader caches from several threads).
// OVERLAPPED on the file's OS handle is positional and leaves the CRT's file position alone.
// ---------------------------------------------------------------------------------------
static inline ssize_t plat_uwp_pread(int fd, void *buf, size_t n, long long off) {
    intptr_t os = _get_osfhandle(fd);
    if (os == -1) { errno = EBADF; return -1; }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((unsigned long long)off & 0xffffffffull);
    ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD got = 0;
    if (!ReadFile((HANDLE)os, buf, (DWORD)n, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return (ssize_t)got;
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

static inline ssize_t plat_uwp_pwrite(int fd, const void *buf, size_t n, long long off) {
    intptr_t os = _get_osfhandle(fd);
    if (os == -1) { errno = EBADF; return -1; }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)((unsigned long long)off & 0xffffffffull);
    ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    DWORD put = 0;
    if (!WriteFile((HANDLE)os, buf, (DWORD)n, &put, &ov)) { errno = EIO; return -1; }
    return (ssize_t)put;
}

#define pread  plat_uwp_pread
#define pwrite plat_uwp_pwrite

// ---------------------------------------------------------------------------------------
// environment, flush, randomness
// ---------------------------------------------------------------------------------------
// SetEnvironmentVariableA is in the UWP allowlist; _putenv_s keeps the CRT's copy in step so
// getenv() in the same process sees the change.
static inline int setenv(const char *k, const char *v, int overwrite) {
    if (!overwrite && getenv(k)) return 0;
    SetEnvironmentVariableA(k, v);
    return _putenv_s(k, v);
}
static inline int unsetenv(const char *k) {
    SetEnvironmentVariableA(k, NULL);
    return _putenv_s(k, "");
}

static inline int fsync(int fd) { return _commit(fd); }

// rand_s needs _CRT_RAND_S in stdlib.h; declare it directly rather than forcing a build-wide
// macro that would also change how the rest of the CRT's prototypes are declared.
#ifndef _CRT_RAND_S
int __cdecl rand_s(unsigned int *v);
#endif
static inline uint32_t arc4random(void) {
    unsigned v = 0;
    rand_s(&v);
    return v;
}
static inline uint32_t arc4random_uniform(uint32_t n) {
    if (n < 2) return 0;
    uint32_t min = (uint32_t)(-n) % n, r;  // reject low values that would bias the modulo
    do r = arc4random(); while (r < min);
    return r % n;
}

// ---------------------------------------------------------------------------------------
// string helpers UCRT lacks
// ---------------------------------------------------------------------------------------
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}
static inline size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
static inline char *strsep(char **sp, const char *delim) {
    char *s = *sp;
    if (!s) return NULL;
    char *e = s + strcspn(s, delim);
    *sp = *e ? e + 1 : NULL;
    *e = 0;
    return s;
}
#if defined(_MSC_VER)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

// POSIX mkdir(path, mode); UCRT's _mkdir takes only the path.
static inline int plat_mkdir(const char *p, int mode) { (void)mode; return _mkdir(p); }
#define mkdir(p, m) plat_mkdir(p, m)

// realpath(path, out[PATH_MAX]): absolute form of an existing path. Symlinks are not resolved;
// no caller here needs that, and GetFinalPathNameByHandleW would need a handle opened with
// FILE_FLAG_BACKUP_SEMANTICS for directories.
static inline char *realpath(const char *p, char *out) {
    struct _stat64 st;
    return _stat64(p, &st) == 0 ? _fullpath(out, p, PATH_MAX) : NULL;
}
