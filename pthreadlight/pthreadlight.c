/*
 * pthreadlight.c — Minimal POSIX-threads-on-Windows implementation.
 *
 * Maps pthread primitives directly to Vista+ native Windows synchronisation:
 *   Mutex    → CRITICAL_SECTION
 *   Condvar  → CONDITION_VARIABLE + SleepConditionVariableCS
 *   Thread   → _beginthreadex / WaitForSingleObject
 *   TLS key  → TlsAlloc
 *   Once     → InterlockedCompareExchange state machine
 *
 * Static initializers (PTHREAD_MUTEX_INITIALIZER etc.) are encoded as
 * negative pointer-sized sentinels that get lazily resolved to real
 * heap objects on first use.
 */

#include "pthreadlight.h"
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Internal structs (opaque — callers only see pointer typedefs)
 * --------------------------------------------------------------------------- */

struct pthread_mutex_s {
    CRITICAL_SECTION cs;
    int              type;       /* PTHREAD_MUTEX_NORMAL / RECURSIVE / ERRORCHECK */
};

struct pthread_cond_s {
    CONDITION_VARIABLE cv;
};

struct pthread_attr_s {
    int detachstate;
    int scope;
};

struct pthread_mutexattr_s {
    int type;
};

/* ---------------------------------------------------------------------------
 * Thread-wrapper: passes user start/arg through _beginthreadex
 * --------------------------------------------------------------------------- */

typedef struct {
    void *(*start)(void *);
    void    *arg;
    pthread_t pt;               /* filled in before the user function runs */
} thwrap_t;

/* ---------------------------------------------------------------------------
 * TLS cleanup stack (used by pthread_cleanup_push/pop macros)
 * --------------------------------------------------------------------------- */

__thread struct _pthread_cleanup_buffer *_pthread_cleanup_top;

/* ------------------------------------------------------------------ */
/* pthreads-win32 compatibility stubs (we use winpthreads now)        */
/* ------------------------------------------------------------------ */
void __ptw32_processInitialize(void) {}
void ptw32_processInitialize(void) {}

/* ---------------------------------------------------------------------------
 * Sentinel helpers — negative pointer-sized values never collide with real
 * heap addresses on Windows x64 (user space: 0 .. 0x00007FFFFFFFFFFF).
 * --------------------------------------------------------------------------- */

static __inline int
is_sentinel_mutex(pthread_mutex_t m)
{
    return (intptr_t) m < 0;
}

static __inline int
is_sentinel_cond(pthread_cond_t c)
{
    return (intptr_t) c < 0;
}

/*
 * Lazily resolve a sentinel mutex to a real heap-allocated object.
 * Returns 0 on success.  Caller must hold no locks.
 */
static int
resolve_mutex(pthread_mutex_t *mutex)
{
    pthread_mutex_t mx;
    int type;
    intptr_t sentinel;

    if (!is_sentinel_mutex(*mutex))
        return 0;               /* already resolved */

    sentinel = (intptr_t) *mutex;
    switch (sentinel) {
    case (intptr_t) -1: type = PTHREAD_MUTEX_DEFAULT;    break;
    case (intptr_t) -2: type = PTHREAD_MUTEX_ERRORCHECK; break;
    case (intptr_t) -3: type = PTHREAD_MUTEX_RECURSIVE;  break;
    default:            return EINVAL;
    }

    mx = calloc(1, sizeof(struct pthread_mutex_s));
    if (!mx) return EINVAL;

    InitializeCriticalSection(&mx->cs);
    mx->type = type;

    /* Try to install our allocation.  If another thread beat us, discard. */
    if (InterlockedCompareExchangePointer((PVOID volatile *) mutex,
                                          mx, (PVOID) sentinel)
        != (PVOID) sentinel) {
        DeleteCriticalSection(&mx->cs);
        free(mx);
    }
    return 0;
}

static int
resolve_cond(pthread_cond_t *cond)
{
    pthread_cond_t cv;
    intptr_t sentinel;

    if (!is_sentinel_cond(*cond))
        return 0;

    sentinel = (intptr_t) *cond;
    if (sentinel != (intptr_t) -1)
        return EINVAL;

    cv = calloc(1, sizeof(struct pthread_cond_s));
    if (!cv) return EINVAL;

    InitializeConditionVariable(&cv->cv);

    if (InterlockedCompareExchangePointer((PVOID volatile *) cond,
                                          cv, (PVOID) sentinel)
        != (PVOID) sentinel) {
        free(cv);
    }
    return 0;
}

/* ========================================================================
 * Mutex
 * ======================================================================== */

int
pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    pthread_mutex_t mx;
    int type = PTHREAD_MUTEX_DEFAULT;

    if (!mutex) return EINVAL;

    if (attr && *attr)
        type = (*attr)->type;

    mx = calloc(1, sizeof(struct pthread_mutex_s));
    if (!mx) return EINVAL;

    InitializeCriticalSection(&mx->cs);
    mx->type = type;
    *mutex   = mx;
    return 0;
}

int
pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    pthread_mutex_t mx;

    if (!mutex) return EINVAL;
    mx = *mutex;
    if (!mx) return EINVAL;

    if (is_sentinel_mutex(mx)) {
        *mutex = NULL;
        return 0;
    }

    DeleteCriticalSection(&mx->cs);
    free(mx);
    *mutex = NULL;
    return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int r;

    if (!mutex) return EINVAL;
    r = resolve_mutex(mutex);
    if (r) return r;

    EnterCriticalSection(&(*mutex)->cs);
    return 0;
}

int
pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    int r;

    if (!mutex) return EINVAL;
    r = resolve_mutex(mutex);
    if (r) return r;

    if (!TryEnterCriticalSection(&(*mutex)->cs))
        return EBUSY;
    return 0;
}

int
pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex || !*mutex || is_sentinel_mutex(*mutex))
        return EINVAL;

    LeaveCriticalSection(&(*mutex)->cs);
    return 0;
}

/* ========================================================================
 * Mutex attributes
 * ======================================================================== */

int
pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    pthread_mutexattr_t a;

    if (!attr) return EINVAL;
    a = calloc(1, sizeof(struct pthread_mutexattr_s));
    if (!a) return EINVAL;
    a->type = PTHREAD_MUTEX_DEFAULT;
    *attr = a;
    return 0;
}

int
pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    if (!attr || !*attr) return EINVAL;
    free(*attr);
    *attr = NULL;
    return 0;
}

int
pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    if (!attr || !*attr) return EINVAL;
    (*attr)->type = type;
    return 0;
}

int
pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    if (!attr || !*attr || !type) return EINVAL;
    *type = (*attr)->type;
    return 0;
}

/* ========================================================================
 * Condition variables
 * ======================================================================== */

int
pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    pthread_cond_t cv;
    (void) attr;

    if (!cond) return EINVAL;

    cv = calloc(1, sizeof(struct pthread_cond_s));
    if (!cv) return EINVAL;

    InitializeConditionVariable(&cv->cv);
    *cond = cv;
    return 0;
}

int
pthread_cond_destroy(pthread_cond_t *cond)
{
    pthread_cond_t cv;

    if (!cond) return EINVAL;
    cv = *cond;
    if (!cv) return EINVAL;

    if (is_sentinel_cond(cv)) {
        *cond = NULL;
        return 0;
    }

    /* CONDITION_VARIABLE does not require explicit destruction */
    free(cv);
    *cond = NULL;
    return 0;
}

int
pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    BOOL ok;

    if (!cond || !*cond || !mutex || !*mutex) return EINVAL;

    /* resolve sentinel cond (mutex already resolved by caller's lock) */
    resolve_cond(cond);

    ok = SleepConditionVariableCS(&(*cond)->cv, &(*mutex)->cs, INFINITE);
    return ok ? 0 : EINVAL;
}

int
pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond || !*cond || is_sentinel_cond(*cond)) return EINVAL;

    WakeConditionVariable(&(*cond)->cv);
    return 0;
}

int
pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond || !*cond || is_sentinel_cond(*cond)) return EINVAL;

    WakeAllConditionVariable(&(*cond)->cv);
    return 0;
}

/* ========================================================================
 * Run all cleanup handlers for the current thread (newest first).
 * ======================================================================== */

void
_pthread_cleanup_run_all(void)
{
    struct _pthread_cleanup_buffer *cb = _pthread_cleanup_top;
    _pthread_cleanup_top = NULL;        /* prevent re-entrancy */
    while (cb) {
        cb->routine(cb->arg);
        cb = cb->prev;
    }
}

/* ========================================================================
 * Threads
 * ======================================================================== */

static unsigned __stdcall
thread_bootstrap(void *arg)
{
    thwrap_t *w = (thwrap_t *) arg;
    void     *result;

    w->pt.tid = GetCurrentThreadId();

    /* Duplicate the pseudo-handle so it survives beyond this function */
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &w->pt.handle,
                    0, FALSE, DUPLICATE_SAME_ACCESS);

    /* run user entry-point */
    result = w->start(w->arg);

    /* run cleanup stack (catches non-pthread_exit returns too) */
    _pthread_cleanup_run_all();

    free(w);
    _endthreadex((unsigned)(uintptr_t) result);
    return 0;                   /* never reached */
}

int
pthread_create(pthread_t *thread, const pthread_attr_t *attr,
               void *(*start_routine)(void *), void *arg)
{
    thwrap_t  *w;
    HANDLE     h;
    unsigned   tid;

    (void) attr;

    if (!thread || !start_routine) return EINVAL;

    w = calloc(1, sizeof(thwrap_t));
    if (!w) return EINVAL;
    w->start = start_routine;
    w->arg   = arg;

    h = (HANDLE) _beginthreadex(NULL, 0, thread_bootstrap, w, 0, &tid);
    if (!h) {
        free(w);
        return EINVAL;
    }

    /* thread_bootstrap fills in w->pt.handle and w->pt.tid,
     * but it DuplicateHandle's on itself — we just need the tid here. */
    w->pt.tid    = tid;
    w->pt.handle = h;

    *thread = w->pt;
    return 0;
}

int
pthread_join(pthread_t thread, void **retval)
{
    DWORD code;
    DWORD r;

    if (!thread.handle) return EINVAL;

    r = WaitForSingleObject(thread.handle, INFINITE);
    if (r != WAIT_OBJECT_0) return EINVAL;

    if (retval) {
        if (GetExitCodeThread(thread.handle, &code))
            *retval = (void *) (uintptr_t) code;
        else
            *retval = NULL;
    }
    CloseHandle(thread.handle);
    return 0;
}

int
pthread_detach(pthread_t thread)
{
    if (thread.handle)
        CloseHandle(thread.handle);
    return 0;
}

void
pthread_exit(void *retval)
{
    _pthread_cleanup_run_all();
    _endthreadex((unsigned)(uintptr_t) retval);
}

pthread_t
pthread_self(void)
{
    pthread_t pt;
    pt.handle = NULL;           /* join on self is invalid */
    pt.tid    = GetCurrentThreadId();
    return pt;
}

int
pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1.tid == t2.tid;
}

int
pthread_cancel(pthread_t thread)
{
    /* Harsh, but vcxsrv never calls this — exists for link compatibility. */
    TerminateThread(thread.handle, 0);
    return 0;
}

/* ========================================================================
 * Thread attributes
 * ======================================================================== */

int
pthread_attr_init(pthread_attr_t *attr)
{
    pthread_attr_t a;

    if (!attr) return EINVAL;
    a = calloc(1, sizeof(struct pthread_attr_s));
    if (!a) return EINVAL;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->scope       = PTHREAD_SCOPE_SYSTEM;
    *attr = a;
    return 0;
}

int
pthread_attr_destroy(pthread_attr_t *attr)
{
    if (!attr || !*attr) return EINVAL;
    free(*attr);
    *attr = NULL;
    return 0;
}

int
pthread_attr_setscope(pthread_attr_t *attr, int scope)
{
    if (!attr || !*attr) return EINVAL;
    (*attr)->scope = scope;
    return 0;
}

int
pthread_attr_getscope(const pthread_attr_t *attr, int *scope)
{
    if (!attr || !*attr || !scope) return EINVAL;
    *scope = (*attr)->scope;
    return 0;
}

int
pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr || !*attr) return EINVAL;
    (*attr)->detachstate = detachstate;
    return 0;
}

int
pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !*attr || !detachstate) return EINVAL;
    *detachstate = (*attr)->detachstate;
    return 0;
}

/* ========================================================================
 * One-time initialisation
 * ======================================================================== */

int
pthread_once(pthread_once_t *once, void (*init_routine)(void))
{
    LONG old;

    if (!once || !init_routine) return EINVAL;

    for (;;) {
        old = InterlockedCompareExchange(&once->state, 1, 0);
        if (old == 2)
            return 0;           /* already completed */
        if (old == 0) {
            /* we own the initialisation */
            init_routine();
            InterlockedExchange(&once->state, 2);
            return 0;
        }
        /* old == 1: another thread is running the init — spin briefly */
        SwitchToThread();
    }
}

/* ========================================================================
 * Thread-local storage keys
 * ======================================================================== */

int
pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    DWORD k;

    (void) destructor;          /* destructors are not called automatically */

    if (!key) return EINVAL;
    k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) return EINVAL;
    *key = k;
    return 0;
}

int
pthread_key_delete(pthread_key_t key)
{
    return TlsFree(key) ? 0 : EINVAL;
}

void *
pthread_getspecific(pthread_key_t key)
{
    return TlsGetValue(key);
}

int
pthread_setspecific(pthread_key_t key, const void *value)
{
    return TlsSetValue(key, (PVOID) value) ? 0 : EINVAL;
}

/* ========================================================================
 * Signal mask — no-op on Windows
 * ======================================================================== */

int
pthread_sigmask(int how, const void *set, void *oldset)
{
    (void) how;
    (void) set;
    (void) oldset;
    return 0;
}

/* ========================================================================
 * Thread naming — no-op
 * ======================================================================== */

int
pthread_setname_np(pthread_t thread, const char *name)
{
    (void) thread;
    (void) name;
    return 0;
}
