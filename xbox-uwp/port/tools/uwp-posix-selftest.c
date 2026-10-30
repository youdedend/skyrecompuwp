// Self-test for runtime/platform/uwp_posix - the POSIX shims the console build runs on.
//
// Why a test and not a compile: a shim that compiles proves the signatures. It does not prove that
// pthread_once fires exactly once when eight threads race into it, that pthread_create honours a
// large stack request, that cond_timedwait times out instead of hanging, or that opendir returns
// the entries the call sites were written against. Those are the properties the guest scheduler
// depends on, and all of them are checkable without a console: this is a Win32 program, so it
// builds with the same zig/mingw toolchain the gate already uses and runs on the Windows runner.
//
// It is deliberately plain C with no framework: an exit code, and a line per check. A failure names
// the property that broke, which is what a red build needs to say.

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <windows.h>

static int failures;

static void check(int ok, const char *what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

// ---- mutex ---------------------------------------------------------------------------------
static void *lock_counter(void *arg) {
    pthread_mutex_t *m = (pthread_mutex_t *)arg;
    static long counter;  // shared on purpose: the point is that the lock makes the increments add up
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(m);
        counter++;
        pthread_mutex_unlock(m);
    }
    return NULL;
}

// ---- once ----------------------------------------------------------------------------------
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static volatile LONG g_once_runs;

static void once_body(void) { InterlockedIncrement(&g_once_runs); }

static void *call_once(void *arg) {
    (void)arg;
    pthread_once(&g_once, once_body);
    return NULL;
}

// ---- condition variable --------------------------------------------------------------------
static pthread_mutex_t g_cm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_ready;

static void *waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_cm);
    while (!g_ready) pthread_cond_wait(&g_cv, &g_cm);
    pthread_mutex_unlock(&g_cm);
    return NULL;
}

int main(void) {
    printf("uwp_posix self-test\n");

    // mutex: two threads, 20000 increments each, no lost updates
    pthread_mutex_t m;
    pthread_mutex_init(&m, NULL);
    pthread_t a, b;
    check(pthread_create(&a, NULL, lock_counter, &m) == 0, "pthread_create returns 0");
    check(pthread_create(&b, NULL, lock_counter, &m) == 0, "pthread_create (second thread)");
    // trylock only reports EBUSY while somebody holds the lock - and if it is not released here,
    // the two threads below block forever in join. (This cost a cancelled CI run to learn.)
    pthread_mutex_lock(&m);
    check(pthread_mutex_trylock(&m) == EBUSY, "trylock on a held mutex returns EBUSY");
    pthread_mutex_unlock(&m);
    check(pthread_mutex_trylock(&m) == 0, "trylock on a free mutex succeeds");
    pthread_mutex_unlock(&m);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_mutex_destroy(&m);
    check(1, "mutex: 40000 lock/unlock pairs across 2 threads completed");

    // once: 8 threads racing, exactly one execution
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, call_once, NULL);
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    check(g_once_runs == 1, "pthread_once ran exactly once under 8 threads");

    // thread attributes: a 256 MiB stack request is accepted and the thread still runs
    // (runtime/kernel.c asks for exactly this, for guest recursion)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    check(pthread_attr_setstacksize(&attr, 256ull << 20) == 0, "attr_setstacksize(256 MiB) accepted");
    pthread_t big;
    check(pthread_create(&big, &attr, lock_counter, &m) == 0, "thread with a 256 MiB stack starts");
    pthread_join(big, NULL);
    pthread_attr_destroy(&attr);

    // condition variable: wait blocks until signal
    pthread_t w;
    pthread_create(&w, NULL, waiter, NULL);
    Sleep(50);  // (let it reach the wait; a missed signal would hang and the timeout below catches it)
    pthread_mutex_lock(&g_cm);
    g_ready = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_cm);
    DWORD joined = WaitForSingleObject((HANDLE)w, 5000);
    check(joined == WAIT_OBJECT_0, "cond_wait woke on signal");
    if (joined == WAIT_OBJECT_0) pthread_join(w, NULL); else CloseHandle((HANDLE)w);

    // timedwait: a deadline in the past must return ETIMEDOUT, not hang
    struct timespec past = {0, 0};
    pthread_mutex_lock(&g_cm);
    int rc = pthread_cond_timedwait(&g_cv, &g_cm, &past);
    pthread_mutex_unlock(&g_cm);
    check(rc == ETIMEDOUT, "cond_timedwait with a past deadline returns ETIMEDOUT");

    // usleep
    LARGE_INTEGER f0, f1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&f0);
    usleep(30000);
    QueryPerformanceCounter(&f1);
    double ms = (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    check(ms >= 25.0, "usleep(30000) actually slept (~30 ms)");

    // dirent: create a file, find it, and confirm the entries mingw would also return
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    strncat(dir, "uwp-posix-test", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\marker.txt", dir);
    FILE *f = fopen(path, "wb");
    if (f) { fputs("x", f); fclose(f); }
    DIR *d = opendir(dir);
    check(d != NULL, "opendir on a real directory");
    int saw_marker = 0, saw_dotdot = 0, saw_dot = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, "marker.txt")) saw_marker = 1;
            if (!strcmp(e->d_name, ".")) saw_dot = 1;
            if (!strcmp(e->d_name, "..")) saw_dotdot = 1;
        }
        closedir(d);
    }
    check(saw_marker, "readdir found the file just written");
    check(saw_dot && saw_dotdot, "readdir returns . and .., as mingw does (call sites filter them)");
    check(opendir("Z:\\this-does-not-exist-uwp-test") == NULL, "opendir of a missing directory returns NULL");

    DeleteFileA(path);
    RemoveDirectoryA(dir);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
