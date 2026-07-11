/* __imp_ aliases for libX11.a's dllimport references.
   The real definitions come from libX11/src/XlibInt.c.
   Also stubs for _XInitOM / _XInitIM previously in xkbcomp_mutex.c. */

#include <stddef.h>

typedef void* LockInfoPtr;
typedef unsigned long xthread_t;

extern void (*_XLockMutex_fn)(LockInfoPtr lip, char *file, int line);
extern void (*_XUnlockMutex_fn)(LockInfoPtr lip, char *file, int line);
extern void (*_XCreateMutex_fn)(LockInfoPtr lip);
extern void (*_XFreeMutex_fn)(LockInfoPtr lip);
extern LockInfoPtr _Xglobal_lock;
extern int  (*_XErrorFunction)(void *dpy, void *err);
extern int  (*_XIOErrorFunction)(void *dpy);
extern xthread_t (*_Xthread_self_fn)(void);

void (**__imp__XLockMutex_fn)(LockInfoPtr lip, char *file, int line) = &_XLockMutex_fn;
void (**__imp__XUnlockMutex_fn)(LockInfoPtr lip, char *file, int line) = &_XUnlockMutex_fn;
void (**__imp__XCreateMutex_fn)(LockInfoPtr lip) = &_XCreateMutex_fn;
void (**__imp__XFreeMutex_fn)(LockInfoPtr lip) = &_XFreeMutex_fn;
LockInfoPtr *__imp__Xglobal_lock = &_Xglobal_lock;
int (**__imp__XErrorFunction)(void *dpy, void *err) = &_XErrorFunction;
int (**__imp__XIOErrorFunction)(void *dpy) = &_XIOErrorFunction;
xthread_t (**__imp__Xthread_self_fn)(void) = &_Xthread_self_fn;

int _XInitOM(void *lcd) { (void)lcd; return 1; }
int _XInitIM(void *lcd) { (void)lcd; return 1; }
