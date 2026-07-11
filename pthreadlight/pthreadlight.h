#ifndef PTHREADLIGHT_H
#define PTHREADLIGHT_H

#include <windows.h>
#include <process.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#undef pthread_attr_destroy
#undef pthread_attr_getdetachstate
#undef pthread_attr_getscope
#undef pthread_attr_init
#undef pthread_attr_setdetachstate
#undef pthread_attr_setscope
#undef pthread_cancel
#undef pthread_cleanup_pop
#undef pthread_cleanup_push
#undef pthread_cond_broadcast
#undef pthread_cond_destroy
#undef pthread_cond_init
#undef pthread_cond_signal
#undef pthread_cond_wait
#undef pthread_create
#undef pthread_detach
#undef pthread_equal
#undef pthread_exit
#undef pthread_getspecific
#undef pthread_join
#undef pthread_key_create
#undef pthread_key_delete
#undef pthread_key_t
#undef pthread_mutexattr_destroy
#undef pthread_mutexattr_gettype
#undef pthread_mutexattr_init
#undef pthread_mutexattr_settype
#undef pthread_mutex_destroy
#undef pthread_mutex_init
#undef pthread_mutex_lock
#undef pthread_mutex_trylock
#undef pthread_mutex_unlock
#undef pthread_once
#undef pthread_once_t
#undef pthread_self
#undef pthread_setname_np
#undef pthread_setspecific
#undef pthread_sigmask
#undef pthread_t
#undef __ptw32_processInitialize
#undef ptw32_processInitialize


void __ptw32_processInitialize(void);
void ptw32_processInitialize(void);

/* ========================================================================
 * Types
 * ======================================================================== */

typedef struct {
    HANDLE handle;
    DWORD  tid;
} pthread_t;

typedef struct pthread_mutex_s  *pthread_mutex_t;
typedef struct pthread_cond_s   *pthread_cond_t;
typedef struct pthread_attr_s   *pthread_attr_t;
typedef struct pthread_mutexattr_s *pthread_mutexattr_t;

typedef DWORD pthread_key_t;

typedef struct {
    LONG state;
} pthread_once_t;

/* ========================================================================
 * Static initializer sentinel values
 *
 * Negative (bit63=1) pointer-sized values that never collide with real
 * heap pointers on Windows x64 (user space is 0 .. 0x00007FFFFFFFFFFF).
 * The encoding also carries the mutex type.
 * ======================================================================== */

#define PTHREAD_MUTEX_INITIALIZER               ((pthread_mutex_t)(intptr_t)-1)
#define PTHREAD_ERRORCHECK_MUTEX_INITIALIZER    ((pthread_mutex_t)(intptr_t)-2)
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP  ((pthread_mutex_t)(intptr_t)-3)

#define PTHREAD_COND_INITIALIZER  ((pthread_cond_t)(intptr_t)-1)
#define PTHREAD_ONCE_INIT         {0}

/* ========================================================================
 * Mutex type constants
 * ======================================================================== */

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

/* ========================================================================
 * Thread attribute constants
 * ======================================================================== */

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1
#define PTHREAD_SCOPE_SYSTEM     0
#define PTHREAD_SCOPE_PROCESS    1
#define PTHREAD_INHERIT_SCHED    0

/* ========================================================================
 * Error codes (match Windows/Pthreads4w values)
 * ======================================================================== */

#define EPERM       1
#define EBUSY      16
#define EINVAL     22
#define ETIMEDOUT 138

/* ========================================================================
 * Mutex
 * ======================================================================== */

int pthread_mutex_init   (pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock   (pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock (pthread_mutex_t *mutex);

/* ========================================================================
 * Mutex attributes
 * ======================================================================== */

int pthread_mutexattr_init    (pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy (pthread_mutexattr_t *attr);
int pthread_mutexattr_settype (pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype (const pthread_mutexattr_t *attr, int *type);

/* ========================================================================
 * Condition variables
 * ======================================================================== */

int pthread_cond_init     (pthread_cond_t *cond, const void *attr);
int pthread_cond_destroy  (pthread_cond_t *cond);
int pthread_cond_wait     (pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal   (pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* ========================================================================
 * Threads
 * ======================================================================== */

int        pthread_create (pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start_routine)(void *), void *arg);
int        pthread_join   (pthread_t thread, void **retval);
int        pthread_detach (pthread_t thread);
void       pthread_exit   (void *retval);
pthread_t  pthread_self   (void);
int        pthread_equal  (pthread_t t1, pthread_t t2);
int        pthread_cancel (pthread_t thread);

/* ========================================================================
 * Thread attributes
 * ======================================================================== */

int pthread_attr_init          (pthread_attr_t *attr);
int pthread_attr_destroy       (pthread_attr_t *attr);
int pthread_attr_setscope      (pthread_attr_t *attr, int scope);
int pthread_attr_getscope      (const pthread_attr_t *attr, int *scope);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);

/* ========================================================================
 * Cleanup handlers (macros + TLS variable)
 * ======================================================================== */

struct _pthread_cleanup_buffer {
    void (*routine)(void *);
    void *arg;
    struct _pthread_cleanup_buffer *prev;
};

extern __thread struct _pthread_cleanup_buffer *_pthread_cleanup_top;

#define pthread_cleanup_push(routine_, arg_) \
    do { \
        struct _pthread_cleanup_buffer _cb; \
        _cb.routine = (void (*)(void*))(routine_); \
        _cb.arg     = (void *)(arg_); \
        _cb.prev    = _pthread_cleanup_top; \
        _pthread_cleanup_top = &_cb;

#define pthread_cleanup_pop(execute) \
        _pthread_cleanup_top = _cb.prev; \
        if (execute) { _cb.routine(_cb.arg); } \
    } while (0)

/* ========================================================================
 * One-time initialisation
 * ======================================================================== */

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

/* ========================================================================
 * Thread-local storage keys
 * ======================================================================== */

int   pthread_key_create (pthread_key_t *key, void (*destructor)(void *));
int   pthread_key_delete (pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int   pthread_setspecific(pthread_key_t key, const void *value);

/* ========================================================================
 * Signals (no-ops on Windows)
 * ======================================================================== */

int pthread_sigmask(int how, const void *set, void *oldset);

/* ========================================================================
 * Thread naming
 * ======================================================================== */

int pthread_setname_np(pthread_t thread, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PTHREADLIGHT_H */
