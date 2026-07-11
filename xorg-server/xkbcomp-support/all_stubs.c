/* Comprehensive stubs for MinGW static linking of VcXsrv */

#include <stddef.h>
#include <stdlib.h>

/* Avoid including Xlib headers -- just declare opaque types */
typedef void* LockInfoPtr;
typedef unsigned long xthread_t;

/* ------------------------------------------------------------------ */
/* libX11 locking/callback __imp_ aliases for dllimport consumers      */
/*                                                                     */
/* libX11.a was compiled with __declspec(dllimport) conventions, so   */
/* it accesses these data symbols through __imp_ pointers.  We define */
/* the real variable AND an __imp_ pointer to it so both direct and   */
/* dllimport-style access work.                                       */
/*                                                                     */
/* Most libX11 internal function stubs are now provided by the real   */
/* libX11.a (rebuilt from source). These __imp_ forwarders and a few  */
/* remaining stubs are all that remain.                                */
/* ------------------------------------------------------------------ */

/* -- The real data variables (BSS in libX11.a's XlibInt.o) -- */
extern void (*_XLockMutex_fn)(LockInfoPtr lip, char *file, int line);
extern void (*_XUnlockMutex_fn)(LockInfoPtr lip, char *file, int line);
extern void (*_XCreateMutex_fn)(LockInfoPtr lip);
extern void (*_XFreeMutex_fn)(LockInfoPtr lip);
extern LockInfoPtr _Xglobal_lock;

extern int  (*_XErrorFunction)(void *dpy, void *err);
extern int  (*_XIOErrorFunction)(void *dpy);

/* -- The __imp_ import pointers for dllimport consumers -- */
void (**__imp__XLockMutex_fn)(LockInfoPtr lip, char *file, int line) = &_XLockMutex_fn;
void (**__imp__XUnlockMutex_fn)(LockInfoPtr lip, char *file, int line) = &_XUnlockMutex_fn;
void (**__imp__XCreateMutex_fn)(LockInfoPtr lip) = &_XCreateMutex_fn;
void (**__imp__XFreeMutex_fn)(LockInfoPtr lip) = &_XFreeMutex_fn;
LockInfoPtr *__imp__Xglobal_lock = &_Xglobal_lock;
int (**__imp__XErrorFunction)(void *dpy, void *err) = &_XErrorFunction;
int (**__imp__XIOErrorFunction)(void *dpy) = &_XIOErrorFunction;

/* ------------------------------------------------------------------ */
/* BSD / POSIX compat                                                 */
/* ------------------------------------------------------------------ */
long random(void) { return rand(); }
void srandom(unsigned seed) { srand(seed); }

/* reallocarray -- defined as xreallocarray in xorg-server, provide plain version */
extern void *xreallocarray(void *optr, size_t nmemb, size_t size);
void *reallocarray(void *optr, size_t nmemb, size_t size) {
    return xreallocarray(optr, nmemb, size);
}
