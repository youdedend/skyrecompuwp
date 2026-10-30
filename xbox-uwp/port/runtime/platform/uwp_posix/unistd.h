// <unistd.h> for UWP.
//
// Ten runtime files include it for the POSIX spelling of a handful of calls. It is not that MSVC
// provides none of them at all - it is that they are named _unlink, _chdir, _getcwd, _access,
// _rmdir. Those mappings, and the 64-bit off_t the runtime needs for romfs.bin, live in
// ../uwp_compat.h; this header pulls them in and adds the few POSIX calls that have no UCRT name
// at all (usleep, getpid).
//
// Including the compat header from here, rather than repeating any of it, matters: several files
// (runtime/nvn/vulkan/device.c among them) include <unistd.h> before anything that reaches
// platform.h, and a duplicate definition of any of the static inline helpers would not compile.
#pragma once

#include "../uwp_compat.h"

#include <process.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// POSIX usleep(3): microseconds, interrupted by nothing, no return value worth checking.
// Sleep takes milliseconds, so anything shorter than 1 ms still sleeps 1 ms - the runtime's calls
// are frame- and retry-paced, never sub-millisecond, and lying about having slept would be worse.
static inline int usleep(unsigned usec) {
    Sleep((DWORD)((usec + 999) / 1000));
    return 0;
}

// getpid is deliberately not here. Nothing in the runtime calls it (census: zero call sites), and
// both UCRT and mingw already declare it in <process.h> - so defining it was a conflict waiting for
// the first translation unit that pulled that header in. A shim should not shadow a function the
// platform already provides; the self-test found this one.

#ifdef __cplusplus
}
#endif
