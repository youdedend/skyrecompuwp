// <pthread.h> for UWP, as a shim over Win32 synchronisation primitives.
//
// Why this exists: the runtime is written against POSIX threads (16 of its files include this
// header, and the guest scheduler depends on real mutexes and condition variables - see
// runtime/kernel.c). Upstream builds for Windows with mingw-w64 headers, which bring winpthreads.
// An AppContainer image has to come out of MSVC/UCRT, which has no <pthread.h> at all, and
// winpthreads is a mingw library that is not built against the UWP CRT.
//
// So the surface is implemented here, over SRW locks, condition variables and CreateThread - all
// of which are in the AppContainer API allowlist. It is header-only on purpose: no extra object or
// library to add to the link line, and no mismatch between what a translation unit saw and what
// the linker got.
//
// Only what the runtime actually uses is here, measured, not guessed:
//   mutex  - init/destroy/lock/trylock/unlock, PTHREAD_MUTEX_INITIALIZER   (20 initialisers)
//   cond   - init/destroy/wait/timedwait/signal/broadcast, initialiser
//   once   - pthread_once, PTHREAD_ONCE_INIT
//   thread - create/join/self, attr init/setstacksize/destroy
// Anything else is deliberately absent: a missing function is a compile error at the call site,
// which is what you want. A silently wrong stub is not.
//
// Differences from pthreads that callers must know about, all of which hold for this runtime:
//   * A mutex is an SRW lock: not recursive, and unlocking from a thread that does not own it is
//     undefined. POSIX allows neither either, so no caller can be relying on it.
//   * pthread_join does not carry the thread's return value. The runtime's threads return NULL and
//     nothing joins with a non-NULL result; a Win32 thread exit code is 32 bits and would truncate
//     a pointer, so transporting it would be worse than not doing it.
//   * pthread_self returns the current thread's pseudo-handle, which is valid for priority and
//     affinity calls but cannot be joined or closed. Nothing here passes it to either.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// --------------------------------------------------------------------------------------- mutex
// SRWLOCK rather than CRITICAL_SECTION: it can be statically initialised, which is what
// PTHREAD_MUTEX_INITIALIZER has to be - 20 of them are file-scope objects in the runtime.
typedef struct {
    SRWLOCK lock;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { SRWLOCK_INIT }

typedef struct {
    int unused;  // PTHREAD_MUTEX_NORMAL only; kept so the type exists for the init signature
} pthread_mutexattr_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    InitializeSRWLock(&m->lock);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;  // nothing to release: an SRW lock has no kernel object
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(&m->lock);
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    return TryAcquireSRWLockExclusive(&m->lock) ? 0 : EBUSY;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}

// ---------------------------------------------------------------------------------- condition
typedef struct {
    CONDITION_VARIABLE cv;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER { CONDITION_VARIABLE_INIT }

typedef struct {
    int unused;
} pthread_condattr_t;

static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    InitializeConditionVariable(&c->cv);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *c) {
    (void)c;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    // The runtime always waits with an infinite timeout; a spurious failure would be worse than a
    // retry, so a false return loops.
    while (!SleepConditionVariableSRW(&c->cv, &m->lock, INFINITE, 0)) {
        if (GetLastError() != ERROR_TIMEOUT) return EINVAL;
    }
    return 0;
}

static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                                         const struct timespec *abstime) {
    // abstime is CLOCK_REALTIME, as POSIX specifies, and SleepConditionVariableSRW wants a
    // relative millisecond count. A deadline already in the past becomes 0, which makes it test
    // the condition once and time out - the behaviour timedwaits are used for.
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    int64_t ms = ((int64_t)abstime->tv_sec - (int64_t)now.tv_sec) * 1000 +
                 ((int64_t)abstime->tv_nsec - (int64_t)now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    if (ms > 0x7fffffff) ms = 0x7fffffff;  // INFINITE is 0xffffffff; stay clear of it
    if (!SleepConditionVariableSRW(&c->cv, &m->lock, (DWORD)ms, 0))
        return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *c) {
    WakeConditionVariable(&c->cv);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    WakeAllConditionVariable(&c->cv);
    return 0;
}

// --------------------------------------------------------------------------------------- once
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

typedef struct {
    void (*fn)(void);
} uwp_once_body;

static inline BOOL CALLBACK uwp_once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)ctx;
    ((uwp_once_body *)param)->fn();  // exactly once, however many threads arrive
    return TRUE;
}

static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    uwp_once_body body = {fn};
    return InitOnceExecuteOnce(once, uwp_once_trampoline, &body, NULL) ? 0 : EINVAL;
}

// ------------------------------------------------------------------------------------- thread
typedef HANDLE pthread_t;

typedef struct {
    size_t stack_size;  // 0: the image's default
} pthread_attr_t;

static inline int pthread_attr_init(pthread_attr_t *a) {
    a->stack_size = 0;
    return 0;
}

static inline int pthread_attr_destroy(pthread_attr_t *a) {
    (void)a;
    return 0;
}

static inline int pthread_attr_setstacksize(pthread_attr_t *a, size_t bytes) {
    a->stack_size = bytes;  // kernel.c asks for 256 MiB: guest recursion runs on the host stack
    return 0;
}

typedef struct {
    void *(*start)(void *);
    void *arg;
} uwp_thread_body;

static inline DWORD WINAPI uwp_thread_trampoline(LPVOID param) {
    uwp_thread_body body = *(uwp_thread_body *)param;
    free(param);
    body.start(body.arg);
    return 0;  // see the note at the top: return values are not transported
}

static inline int pthread_create(pthread_t *t, const pthread_attr_t *attr,
                                 void *(*start)(void *), void *arg) {
    uwp_thread_body *body = malloc(sizeof *body);
    if (!body) return EAGAIN;
    body->start = start;
    body->arg = arg;
    HANDLE h = CreateThread(NULL, attr ? (SIZE_T)attr->stack_size : 0, uwp_thread_trampoline, body,
                            0, NULL);
    if (!h) {
        free(body);
        return EAGAIN;
    }
    *t = h;
    return 0;
}

static inline int pthread_join(pthread_t t, void **result) {
    if (result) *result = NULL;
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) return EINVAL;
    CloseHandle(t);
    return 0;
}

static inline pthread_t pthread_self(void) { return GetCurrentThread(); }

// sched_yield lives in <sched.h> by POSIX, but mingw's winpthreads declares it here, and that is
// where runtime/kernel.c has always got it from: the guest scheduler yields when its run queue is
// empty. SwitchToThread hands the rest of the timeslice to another runnable thread on this core;
// when there is none it returns immediately, which is what POSIX allows.
static inline int sched_yield(void) {
    SwitchToThread();
    return 0;
}

#ifdef __cplusplus
}
#endif
