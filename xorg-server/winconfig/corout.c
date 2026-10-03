/* corout.h - Paul Sheer <paulsheer@gmail.com> */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_SETJMP
#include <setjmp.h>
#endif
#ifdef __sun
#include <sys/filio.h>
#endif
#include <time.h>
#include <signal.h>
#ifdef USE_OS_POLLING
#include <sys/resource.h>
#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__sun)
#include <sys/devpoll.h>
#endif
#endif

#include "usertime.h"
#include "ranktree.h"
#include "pairheap.h"
#define COROUT_C
#include "corout.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <wchar.h>
#else
#include <net/if.h>
#endif

#ifdef MSWIN_OVLPIO
struct overlapped;
#endif

struct sockbuf {
    struct buffer *bufrd, *bufwr;
    struct corout_item *c;
    struct socket *e;
    socket_t s;
#ifdef MSWIN_OVLPIO
    int family;
    struct overlapped *overlapped_send;
    struct overlapped *overlapped_recv;
    struct overlapped *overlapped_connect;
    struct overlapped *overlapped_accept;
    struct overlapped *overlapped_disconnect;
#endif
    char accepting;
    char connecting;
    char disconnecting;
#if defined(USE_OS_POLLING) || !defined(MSWIN_OVLPIO)
    struct ranktree_node node_sock;
#endif
};


static int got_SIGINT = 0;




#ifdef _WIN32
static char *wchar_to_char (const wchar_t * w)
{
    char *r;
    size_t c, n;
    n = wcslen (w) + 2;
    r = (char *) malloc (n * 2);
    c = 0;
    wcstombs_s (&c, r, n * 2, w, _TRUNCATE);
    r[c] = '\0';
    return r;
}

void corout_error_str(const int error, char *r, const int n)
{
    wchar_t *s = NULL;
    char *p;
    int l;

    FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, error, MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR) & s, 0, NULL);

    if (!s) {
        strcpy(r, "unknown error");
        return;
    }
    p = wchar_to_char (s);
    LocalFree (s);
    l = strlen (p);
    if (l > n - 1)
        l = n - 1;
    memcpy (r, p, l);
    r[l] = '\0';
    free (p);
/* strip trailing whitespace, newlines, and carriage returns: */
    while (l >= 0 && ((unsigned char *) r)[l] <= ' ')
        r[l--] = '\0';
}

static char *mswin_error_to_text(const int error)
{
    static char r[256];
    corout_error_str(error, r, sizeof(r));
    return r;
}

static void fatal(const int user, const char *fmt,...)
{
    int wsa_error, error;
    char wsaerrs[256], errs[256], msg[256];
    va_list ap;
    if (!user) {
        wsa_error = WSAGetLastError();
        error = GetLastError();
        corout_error_str(wsa_error, wsaerrs, sizeof(wsaerrs));
        corout_error_str(error, errs, sizeof(errs));
    }
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (user)
        fprintf(stderr, "%s\n", msg);
    else
        fprintf(stderr, "%s: %d=>[%s] %d=>[%s]\n", msg, wsa_error, wsaerrs, error, errs);
    exit(1);
}

#else  /* _WIN32 */

void corout_error_str(const int error, char *s, const int n)
{
    strncpy(s, strerror(error), n);
    s[n - 1] = '\0';
}

static void fatal(const int user, const char *fmt,...)
{
    int error = 0;
    char errs[256], msg[256];
    va_list ap;
    if (!user) {
        error = errno;
        strncpy(errs, strerror(error), 255);
        errs[255] = '\0';
    }
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (user)
        fprintf(stderr, "%s\n", msg);
    else
        fprintf(stderr, "%s: %d=>[%s]\n", msg, error, errs);
    exit(1);
}

#endif  /* !_WIN32 */


struct corout_item {
    struct corout *o;
    struct tiny_frame stack[COROUT_MAX_STACK_DEPTH];
    int depth;
    enum corout_result result;
#ifdef HAVE_SETJMP
    jmp_buf rollup;
#endif
    corout_fn_t fn;
    corout_free_fn_t free_fn;
    long long timer;
    int wakeable;   /* timer is an idle poll; safe to fire early on a wakeup */
#define N_SOCKET        10
    struct sockbuf *s[N_SOCKET];
    int n_s;
    void *user_data;
    unsigned long user_signal;
    struct ranktree_node node_user;
    struct pairheap_node node_heap;
};



#ifdef USE_OS_POLLING

#define BIG_FD_SET_CHUNK_SIZE   64
#define BIG_FD_SET_N_CHUNKS     64
#define BIG_FD_SET_N_WORDS      (BIG_FD_SET_CHUNK_SIZE * BIG_FD_SET_N_CHUNKS)
#define BIG_FD_SET_WORD_BITS    (sizeof(size_t) * 8)
#define BIG_FD_SET_CHUNK_BITS   (BIG_FD_SET_CHUNK_SIZE * BIG_FD_SET_WORD_BITS)
#define BIG_FD_SET_WORD_MOD     (BIG_FD_SET_WORD_BITS - 1)
#define BIG_FD_SET_MAX_FD       (BIG_FD_SET_N_WORDS * BIG_FD_SET_WORD_BITS)
#undef FD_SETSIZE
#define FD_SETSIZE              BIG_FD_SET_MAX_FD

struct big_fd_set {
    size_t d[BIG_FD_SET_N_WORDS];
    unsigned long long setops[BIG_FD_SET_N_CHUNKS];
    unsigned long long clrops[BIG_FD_SET_N_CHUNKS];
};

#undef FD_ZERO
#define FD_ZERO(s)      memset((s), '\0', sizeof(struct big_fd_set))
#undef FD_SET
#define FD_SET(fd,s) \
    do { \
        (s)->d[(fd) / BIG_FD_SET_WORD_BITS] |= ((size_t) 1 << ((fd) & BIG_FD_SET_WORD_MOD)); \
        (s)->setops[(fd) / BIG_FD_SET_CHUNK_BITS]++; \
    } while(0)
#undef FD_CLR
#define FD_CLR(fd,s) \
    do { \
        (s)->d[(fd) / BIG_FD_SET_WORD_BITS] &= ~((size_t) 1 << ((fd) & BIG_FD_SET_WORD_MOD)); \
        (s)->clrops[(fd) / BIG_FD_SET_CHUNK_BITS]++; \
    } while(0)
#undef FD_ISSET
#define FD_ISSET(fd,s)  (((s)->d[(fd) / BIG_FD_SET_WORD_BITS] & ((size_t) 1 << ((fd) & BIG_FD_SET_WORD_MOD))) != 0)

#endif


struct corout {
    struct ranktree *u;
    struct pairheap *h;
#ifdef USE_OS_POLLING
    struct ranktree *l;
    struct big_fd_set rd, wr;
    struct big_fd_set old_rd, old_wr;
    socket_t max_fd;
    socket_t old_max_fd;
#ifdef __linux__
    int epoll_fd;
#elif defined(__sun)
    int devpoll_fd;
#endif
#elif defined(MSWIN_OVLPIO)
    HANDLE iocp_handle;
    struct overlapped *completed_list;
#else
    struct ranktree *l;
    fd_set rd, wr;
    socket_t max_fd;
#endif
    int sock_buf_size;
};

#ifndef MSWIN_OVLPIO
static int sock_cmp(void *user_data, const void *a_, const void *b_)
{
    const struct sockbuf *a, *b;
    (void) user_data;
    a = (const struct sockbuf *) a_;
    b = (const struct sockbuf *) b_;
    if (a->s == b->s)
        return 0;
    return a->s > b->s ? 1 : -1;
}
#endif

static int user_cmp(void *user_data, const void *a_, const void *b_)
{
    const struct corout_item *a, *b;
    (void) user_data;
    a = (const struct corout_item *) a_;
    b = (const struct corout_item *) b_;
    if (a->user_data == b->user_data)
        return 0;
    return a->user_data > b->user_data ? 1 : -1;
}

static int timer_cmp(void *user_data, const void *a_, const void *b_)
{
    const struct corout_item *a, *b;
    (void) user_data;
    a = (const struct corout_item *) a_;
    b = (const struct corout_item *) b_;
    if (a->timer == b->timer)
        return 0;
    return a->timer > b->timer ? 1 : -1;
}

#ifdef MSWIN_OVLPIO
static int overlapped_deref(struct overlapped *u, const int clean);

typedef WINBOOL (WINAPI *LPFN_DISCONNECTEX)(SOCKET s,LPOVERLAPPED lpOverlapped,DWORD dwFlags,DWORD dwReserved);
typedef WINBOOL (WINAPI *LPFN_CONNECTEX)(SOCKET s,const struct sockaddr *name,int namelen,PVOID lpSendBuffer,DWORD dwSendDataLength,LPDWORD lpdwBytesSent,LPOVERLAPPED lpOverlapped);
static LPFN_DISCONNECTEX dcfn = NULL;
static LPFN_CONNECTEX cfn = NULL;
#endif

static int corout_init_called = 0;

void corout_init(void)
{
    assert(!corout_init_called);
    corout_init_called++;

#ifdef _WIN32
    WSADATA wsaData;
    memset(&wsaData, '\0', sizeof(wsaData));
    if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
        fprintf(stderr, "WSAStartup error\n");
        exit(1);
    }
#endif  /* _WIN32 */

#ifdef MSWIN_OVLPIO
    DWORD b;
    GUID gcfb = WSAID_CONNECTEX;
    GUID gdcfb = WSAID_DISCONNECTEX;
    socket_t s;
    if ((s = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED)) == OS_SOCK_ERR)
        fatal(0, "WSASocket failed");
    if (WSAIoctl (s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gdcfb, sizeof (gdcfb), &dcfn, sizeof (dcfn), &b, NULL, NULL) == SOCKET_ERROR)
        fatal(0, "lookup DISCONNECTEX failed");
    if (WSAIoctl (s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gcfb, sizeof (gcfb), &cfn, sizeof (cfn), &b, NULL, NULL) == SOCKET_ERROR)
        fatal(0, "lookup CONNECTEX failed");
    closesocket(s);
    assert(cfn);
    assert(dcfn);
#endif

#ifdef USE_OS_POLLING
    struct rlimit rl;
    int r;
    memset(&rl, '\0', sizeof(rl));
    r = getrlimit(RLIMIT_NOFILE, &rl);
    if (r) {
        perror("getrlimit(RLIMIT_NOFILE)");
    } else {
        rl.rlim_cur = rl.rlim_max = 262144;
        r = setrlimit(RLIMIT_NOFILE, &rl);
        if (r)
            perror("setrlimit(RLIMIT_NOFILE)");
    }
#endif

}

struct corout *corout_alloc(void)
{
    struct corout *o;
    assert(corout_init_called == 1);
    o = (struct corout *) malloc(sizeof(*o));
    memset(o, '\0', sizeof(*o));
    o->u = ranktree_alloc(user_cmp, NULL, offsetof(struct corout_item, node_user));
    /* setup to return the lowest numbered corout_item->timer: */
    o->h = pairheap_alloc(timer_cmp, NULL, offsetof(struct corout_item, node_heap));
#ifdef USE_OS_POLLING
    o->l = ranktree_alloc(sock_cmp, NULL, offsetof(struct sockbuf, node_sock));
    FD_ZERO(&o->rd);
    FD_ZERO(&o->wr);
    FD_ZERO(&o->old_rd);
    FD_ZERO(&o->old_wr);
#ifdef __linux__
    o->epoll_fd = epoll_create(BIG_FD_SET_MAX_FD);
    assert(o->epoll_fd != -1);
#elif defined(__sun)
    o->devpoll_fd = open("/dev/poll", O_RDWR);
    assert(o->devpoll_fd != -1);
#endif
#elif defined(MSWIN_OVLPIO)
    o->iocp_handle = CreateIoCompletionPort ((HANDLE) INVALID_HANDLE_VALUE, (HANDLE) NULL, (ULONG_PTR) NULL, 1);
    if (!o->iocp_handle)
        fatal(0, "CreateIoCompletionPort failed");
#else
    o->l = ranktree_alloc(sock_cmp, NULL, offsetof(struct sockbuf, node_sock));
    FD_ZERO(&o->rd);
    FD_ZERO(&o->wr);
#endif
    o->sock_buf_size = 65536;
    return o;
}

#ifndef MSWIN_OVLPIO
static void corout_update_max_fd(struct corout *o, struct sockbuf *s, const int addfd)
{
    struct sockbuf *last, *f;

    if (addfd && !ranktree_is_linked(&s->node_sock)) {
        f = (struct sockbuf *) ranktree_insert(o->l, s);
        assert(!f);
    }
    if (!addfd && ranktree_is_linked(&s->node_sock))
        ranktree_remove(o->l, &s->node_sock);
    last = (struct sockbuf *) ranktree_rindex(o->l, 0);
    o->max_fd = last ? last->s : OS_SOCK_ERR;
}
#endif

/* make TCP buffer sizes smaller when the number of coroutines is large: */
static void adjust_sock_buf_size(struct corout *o)
{
    size_t c;
    o->sock_buf_size = 65536;
    c = ranktree_count(o->u);
    while (c > 128 && o->sock_buf_size > 1024) {
        c >>= 1;
        o->sock_buf_size >>= 1;
    }
}

static void corout_socket_free__(struct sockbuf *s);

void corout_item_remove(struct corout_item *p)
{
    struct corout *o = p->o;
    int i;

    for (i = 0; i < p->n_s; i++) {
        if (!p->s[i])
            continue;
        corout_socket_free__(p->s[i]);
        p->s[i] = NULL;
    }
    ranktree_remove(o->u, &p->node_user);
    adjust_sock_buf_size(o);
    if (p->timer)
        pairheap_remove(o->h, &p->node_heap);
    if (p->free_fn)
        (*p->free_fn) (p->user_data);
    free(p);
}

#ifdef MSWIN_OVLPIO
static int corout_once_overlapped(struct corout *o, const long long t);
static void corout_socket_disconnect(struct sockbuf *s);

static void corout_item_disconnect(struct corout_item *p)
{
    int i;

    for (i = 0; i < p->n_s; i++) {
        if (!p->s[i])
            continue;
        corout_socket_disconnect(p->s[i]);
    }
}
#endif

void corout_free(struct corout *o)
{
    struct ranktree_iterator *i;
    struct corout_item *p;
    i = ranktree_iterator_alloc(o->u);
#ifdef MSWIN_OVLPIO
    for (p = (struct corout_item *) ranktree_iterator_first(i, NULL);
         p; p = (struct corout_item *) ranktree_iterator_next(i))
        corout_item_disconnect(p);
    while (corout_once_overlapped(o, 0));
    CloseHandle(o->iocp_handle);
    o->iocp_handle = NULL;
#endif
    for (p = (struct corout_item *) ranktree_iterator_first(i, NULL);
         p; p = (struct corout_item *) ranktree_iterator_next(i))
        corout_item_remove(p);
    ranktree_iterator_free(i);
#ifndef MSWIN_OVLPIO
    ranktree_free(o->l);
#endif
    ranktree_free(o->u);
    pairheap_free(o->h);
#ifdef USE_OS_POLLING
#ifdef __linux__
    close(o->epoll_fd);
#elif defined(__sun)
    close(o->devpoll_fd);
#endif
#endif
    free(o);
}

void corout_wait_timeout(struct corout_item *p, const unsigned long ms, const int clear_io_waits)
{
    int i;
    if (clear_io_waits)
        for (i = 0; i < p->n_s; i++)
            if (p->s[i])
                corout_clear(p->s[i]->e);
    assert (!p->timer);
    p->wakeable = 0;
    p->timer = millitime() + ms;
    pairheap_insert(p->o->h, p);
}

void corout_wait_wakeable(struct corout_item *p, const unsigned long ms)
{
    assert (!p->timer);
    p->wakeable = 1;
    p->timer = millitime() + ms;
    pairheap_insert(p->o->h, p);
}

static void corout_step(struct corout_item *p, struct sockbuf *s, const int ev, struct accept_sock *as)
{
    struct sockevent g;
    if (p->timer) {
        p->timer = 0;
        pairheap_remove(p->o->h, &p->node_heap);
    }
    g.ev = ev;
    g.e = s ? s->e : NULL;
    g.accept_sock = as;
#ifdef HAVE_SETJMP
    if (!setjmp(p->rollup))
        (*p->fn) (p, p->user_data, &g);
#else
    (*p->fn) (p, p->user_data, &g);
#endif
    assert(!p->depth);
    if (p->result == COROUT_ERROR) {
        corout_item_remove(p);
        return;
    }
}

void corout_signal(struct corout *o, void *user_data, const unsigned long user_signal)
{
    struct corout_item *p, search;

    search.user_data = user_data;
    p = (struct corout_item *) ranktree_find(o->u, &search);
    assert(p && "coroutine not found on signal");
    p->user_signal = user_signal;
    if (p->timer)
        corout_step(p, NULL, CO_EV_SIG, NULL);
}

void corout_get_signal(unsigned long *sig, struct corout_item *c)
{
    if (sig)
        *sig = c->user_signal;
}

#ifdef _WIN32
HANDLE corout_iocp_handle(struct corout *o)
{
#ifdef MSWIN_OVLPIO
    return o->iocp_handle;
#else
    (void) o;
    return NULL;
#endif
}
#endif

void corout_kill(struct corout *o, void *user_data)
{
    struct corout_item *p, search;

    search.user_data = user_data;
    p = (struct corout_item *) ranktree_find(o->u, &search);
    if (!p)
        return;
    corout_item_remove(p);
}

int corout_add(struct corout *o, corout_fn_t fn, corout_free_fn_t free_fn, void *user_data)
{
    struct corout_item *p, *r;

    p = (struct corout_item *) malloc(sizeof(*p));
    memset(p, '\0', sizeof(*p));
    p->o = o, p->fn = fn, p->free_fn = free_fn;
    p->user_data = user_data;
    r = (struct corout_item *) ranktree_insert(o->u, p);
    assert(!r);
    adjust_sock_buf_size(o);
    corout_step(p, NULL, CO_EV_START, NULL);
    return 0;
}

int corout_count(struct corout *o)
{
    return ranktree_count(o->u);
}

#ifdef MSWIN_OVLPIO
struct overlapped_ {
    struct _OVERLAPPED overlapped;
    WSABUF d;
    DWORD flags;
    DWORD l;
    long err;
};

struct overlapped_accept {
    char accept_buf[sizeof(union sockaddr_in4in6) * 2];
    socket_t accept_sock;
};

struct overlapped_connect {
    union sockaddr_in4in6 connect_addr;
};

union overlapped_par {
    struct overlapped_accept ua;
    struct overlapped_connect uc;
};

struct overlapped {
    struct overlapped_ w;
#define OVERLAPPED_MAGIC        0xf91982f5
    unsigned int magic;
    int ref;
    socket_t closable;
    struct socket *e;
    struct buffer *refbuf;
    union overlapped_par u;
    struct overlapped *completed_next;
};

static struct overlapped *overlapped_alloc(struct socket *e)
{
    struct overlapped *u;
    u = (struct overlapped *) malloc(sizeof(struct overlapped));
    memset(u, '\0', sizeof(*u));
    u->magic = OVERLAPPED_MAGIC;
    u->ref = 1;
    u->closable = OS_SOCK_ERR;
    u->e = e;
    return u;
}

static int overlapped_deref(struct overlapped *u, const int clean)
{
    if (!u)
        return 1;
    assert(u->magic == OVERLAPPED_MAGIC);
    if (clean)
        u->e = NULL;
    assert(u->ref >= 0);
    if (!--u->ref) {
        if (u->closable != OS_SOCK_ERR)
            closesocket(u->closable);
        if (u->refbuf) {
            corout_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        u->magic = 0;
        free(u);
        return 1;
    }
    return 0;
}

static void corout_socket_disconnect(struct sockbuf *s)
{
    struct overlapped *u;
    int r;

    if (s->accepting && s->overlapped_accept) {
        if (s->overlapped_accept->u.ua.accept_sock != OS_SOCK_ERR) {
            closesocket(s->overlapped_accept->u.ua.accept_sock);
            s->overlapped_accept->u.ua.accept_sock = -1;
        }
    }

    if (!s->disconnecting) {
        assert (!s->overlapped_disconnect);
        assert (!s->disconnecting);
        s->overlapped_disconnect = overlapped_alloc(s->e);
        u = s->overlapped_disconnect;
        assert(u->ref == 1);
        memset (&u->w, '\0', sizeof (u->w));
        u->ref++;
        u->closable = s->s;
        r = dcfn (s->s, &u->w.overlapped, 0, 0);
/*
        r = DisconnectEx (s->s, &u->w.overlapped, 0, 0);
*/
        if (!r)
            u->w.err = WSAGetLastError ();
        if (r || (!r && u->w.err != ERROR_IO_PENDING)) {
            assert(!u->completed_next);
            u->completed_next = s->c->o->completed_list;
            s->c->o->completed_list = u;
        }
        s->disconnecting = 1;
    }
}
#endif

#ifdef USE_OS_POLLING
static void remove_os_poll(struct corout *o, socket_t fd);
#endif

static void corout_socket_free__(struct sockbuf *s)
{
    if (s->e->unlinked == 1) {
        assert(s->c == NULL);
        shutdown(s->s, 2);
        closesocket(s->s);
        s->s = OS_SOCK_ERR;
    } else {
        assert(s->c);
#ifdef USE_OS_POLLING
        assert (s->c->o);
        remove_os_poll(s->c->o, s->s);      /* or else shutdown creates a mess */
        corout_clear(s->e);
        shutdown(s->s, 2);
        closesocket(s->s);
        s->s = OS_SOCK_ERR;
#elif defined(MSWIN_OVLPIO)
        corout_socket_disconnect(s);
        overlapped_deref(s->overlapped_send, 1);
        overlapped_deref(s->overlapped_recv, 1);
        overlapped_deref(s->overlapped_connect, 1);
        overlapped_deref(s->overlapped_accept, 1);
        overlapped_deref(s->overlapped_disconnect, 1);
#else
        corout_clear(s->e);
        shutdown(s->s, 2);
        closesocket(s->s);
        s->s = OS_SOCK_ERR;
#endif
    }
    corout_buffer_free(s->bufrd);
    corout_buffer_free(s->bufwr);
    s->e->s = NULL;
    free(s);
}

void corout_readwrite(struct socket *e)
{
#ifdef MSWIN_OVLPIO
    corout_write(e);
    corout_read(e);
#else
    struct sockbuf *s = e->s;
    if (s->bufrd->written == s->bufrd->avail)
        s->bufrd->written = s->bufrd->avail = 0;
    assert(s->bufrd->avail < s->bufrd->alloced);
    assert(s->bufwr->written < s->bufwr->avail);
    if (!s->bufrd->reading) {
        FD_SET(s->s, &s->c->o->rd);
        s->bufrd->reading = 1;
    }
    if (!s->bufwr->writing) {
        FD_SET(s->s, &s->c->o->wr);
        s->bufwr->writing = 1;
    }
    s->accepting = s->connecting = 0;
    corout_update_max_fd(s->c->o, s, 1);
#endif
}

void corout_read(struct socket *e)
{
    struct sockbuf *s = e->s;
#ifdef MSWIN_OVLPIO
    if (!s->bufrd->reading) {
        struct overlapped *u;
        int r;

        if (!s->bufrd->writing)  /* don't make changes while an overlapped operation is in progress */
            if (s->bufrd->written == s->bufrd->avail)
                s->bufrd->written = s->bufrd->avail = 0;
        assert(s->bufrd->avail < s->bufrd->alloced);

        if (!e->s->overlapped_recv)
            e->s->overlapped_recv = overlapped_alloc(e);
        u = e->s->overlapped_recv;
        assert(u->ref == 1);
        memset (&u->w, '\0', sizeof (u->w));
        assert(!u->refbuf);
        u->refbuf = s->bufrd;
        u->refbuf->ref++;
        u->w.d.buf = s->bufrd->data + s->bufrd->avail;
        u->w.d.len = s->bufrd->alloced - s->bufrd->avail;
        assert(u->w.d.len);
        u->ref++;

        r = WSARecv (s->s, &u->w.d, 1, &u->w.l, &u->w.flags, &u->w.overlapped, NULL);
        if (r == SOCKET_ERROR)
            u->w.err = WSAGetLastError ();

        if ((r == SOCKET_ERROR && u->w.err != ERROR_IO_PENDING)) {
            assert(!u->completed_next);
            u->completed_next = s->c->o->completed_list;
            s->c->o->completed_list = u;
        }
        s->bufrd->reading = 1;
    }
#else    
    if (s->bufrd->written == s->bufrd->avail)
        s->bufrd->written = s->bufrd->avail = 0;
    assert(s->bufrd->avail < s->bufrd->alloced);
    if (!s->bufrd->reading) {
        FD_SET(s->s, &s->c->o->rd);
        s->bufrd->reading = 1;
    }
    if (s->bufwr->writing) {
        FD_CLR(s->s, &s->c->o->wr);
        s->bufwr->writing = 0;
    }
    s->accepting = s->connecting = 0;
    corout_update_max_fd(s->c->o, s, 1);
#endif
}

void corout_write(struct socket *e)
{
    struct sockbuf *s = e->s;
    assert(s);
    assert(s->bufwr->written < s->bufwr->avail);
#ifdef MSWIN_OVLPIO
    if (!s->bufwr->writing) {
        struct overlapped *u;
        int r;

        if (!e->s->overlapped_send)
            e->s->overlapped_send = overlapped_alloc(e);
        u = e->s->overlapped_send;
        assert(u->ref == 1);
        memset (&u->w, '\0', sizeof (u->w));
        assert(!u->refbuf);
        u->refbuf = s->bufwr;
        u->refbuf->ref++;
        u->w.d.buf = s->bufwr->data + s->bufwr->written;
        u->w.d.len = s->bufwr->avail - s->bufwr->written;
        u->ref++;

        r = WSASend (s->s, &u->w.d, 1, &u->w.l, u->w.flags, &u->w.overlapped, NULL);
        if (r == SOCKET_ERROR)
            u->w.err = WSAGetLastError ();
        if (r == SOCKET_ERROR && u->w.err != ERROR_IO_PENDING) {
            assert(!u->completed_next);
            u->completed_next = s->c->o->completed_list;
            s->c->o->completed_list = u;
        }
        s->bufwr->writing = 1;
    }
#else
    if (s->bufrd->reading) {
        FD_CLR(s->s, &s->c->o->rd);
        s->bufrd->reading = 0;
    }
    if (!s->bufwr->writing) {
        FD_SET(s->s, &s->c->o->wr);
        s->bufwr->writing = 1;
    }
    s->accepting = s->connecting = 0;
    corout_update_max_fd(s->c->o, s, 1);
#endif
}

char *inaddr_str(union sockaddr_in4in6 *a, char *str, int *port)
{
    *str = '\0';
    if (a->sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &a->sain6.sin6_addr, str, 64);
        if (port)
            *port = ntohs(a->sain6.sin6_port);
    } else {
        inet_ntop(AF_INET, &a->sain4.sin_addr, str, 64);
        if (port)
            *port = ntohs(a->sain4.sin_port);
    }
    return str;
}

int inaddr_cmp(union sockaddr_in4in6 *a, union sockaddr_in4in6 *b)
{
    if (a->sa.sa_family != b->sa.sa_family)
        return a->sa.sa_family > b->sa.sa_family ? 1 : -1;
    if (a->sa.sa_family == AF_INET6)
        return memcmp(&a->sain6.sin6_addr, &b->sain6.sin6_addr, sizeof(a->sain6.sin6_addr));
    return memcmp(&a->sain4.sin_addr, &b->sain4.sin_addr, sizeof(a->sain4.sin_addr));
}

static int inaddr_len(union sockaddr_in4in6 *r)
{
    if (r->sa.sa_family == AF_INET6)
        return sizeof(r->sain6);
    return sizeof(r->sain4);
}

/* Resolve a "%zone" suffix to an IPv6 scope id (interface index). A decimal
   string is taken as an index directly; otherwise the zone is matched against
   the adapter's GUID (AdapterName), friendly name, or description. Returns 0
   if the zone cannot be resolved. */
static unsigned long
inaddr_scope_id(const char *zone)
{
    if (*zone && strspn(zone, "0123456789") == strlen(zone))
        return strtoul(zone, NULL, 10);

#ifdef _WIN32
    {
        wchar_t *wzone = NULL;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, zone, -1, NULL, 0);
        ULONG buflen = 0;
        IP_ADAPTER_ADDRESSES *list = NULL, *a;
        unsigned long idx = 0;

        if (wlen > 0)
            wzone = (wchar_t *) malloc((size_t) wlen * sizeof(wchar_t));
        if (wzone)
            MultiByteToWideChar(CP_UTF8, 0, zone, -1, wzone, wlen);

        if (GetAdaptersAddresses(AF_INET6, 0, NULL, NULL, &buflen) ==
                ERROR_BUFFER_OVERFLOW) {
            list = (IP_ADAPTER_ADDRESSES *) malloc(buflen);
            if (list &&
                GetAdaptersAddresses(AF_INET6, 0, NULL, list, &buflen) ==
                    NO_ERROR) {
                for (a = list; a; a = a->Next) {
                    if ((a->AdapterName &&
                         _stricmp(a->AdapterName, zone) == 0) ||
                        (wzone &&
                         ((a->FriendlyName &&
                           _wcsicmp(a->FriendlyName, wzone) == 0) ||
                          (a->Description &&
                           _wcsicmp(a->Description, wzone) == 0)))) {
                        idx = a->Ipv6IfIndex;
                        break;
                    }
                }
            }
        }

        free(wzone);
        free(list);
        return idx;
    }
#else
    return (unsigned long) if_nametoindex(zone);
#endif
}

/* Parse "addr" or "addr%zone" into an IPv6 address, resolving any zone suffix
   to a scope id. Returns 0 on success. */
static int
inaddr_parse_ipv6(const char *s, struct in6_addr *out, unsigned long *scope)
{
    const char *pct = strchr(s, '%');
    unsigned long sc = 0;

    if (pct) {
        char addr[64];
        size_t n = (size_t) (pct - s);

        if (n >= sizeof(addr))
            return 1;
        memcpy(addr, s, n);
        addr[n] = '\0';
        if (!inet_pton(AF_INET6, addr, out))
            return 1;
        sc = inaddr_scope_id(pct + 1);
        if (!sc)
            return 1;
    } else if (!inet_pton(AF_INET6, s, out)) {
        return 1;
    }

    if (scope)
        *scope = sc;
    return 0;
}

/* returns 0 on success */
int inaddr_validate(const char *s, int *family)
{
    if (strchr(s, ':')) {
        struct in6_addr tmp;
        if (inaddr_parse_ipv6(s, &tmp, NULL))
            return 1;
        if (family)
            *family = AF_INET6;
    } else {
        struct in_addr tmp;
        if (!inet_pton(AF_INET, s, &tmp))
            return 1;
        if (family)
            *family = AF_INET;
    }
    return 0;
}

void inaddr_from_text(union sockaddr_in4in6 *r, const char *s, const long port, const int family)
{
    if (port > 65535)
        fatal(1, "invalid port %ld", port);
    memset(r, '\0', sizeof(*r));
    if ((!s && family == AF_INET6) || (s && strchr(s, ':'))) {
        unsigned long sc = 0;
        r->sain6.sin6_family = AF_INET6;
        r->sain6.sin6_port = htons((unsigned short) port);
        if (s && inaddr_parse_ipv6(s, &r->sain6.sin6_addr, &sc))
            fatal(1, "invalid ip %s", s);
        r->sain6.sin6_scope_id = sc;
    } else {
        r->sain4.sin_family = AF_INET;
        r->sain4.sin_port = htons((unsigned short) port);
        if (s && !inet_pton(AF_INET, s, &r->sain4.sin_addr))
            fatal(1, "invalid ip %s", s);
    }
}

int corout_connect(struct socket *e, const long port, const char *saddr)
{
    struct sockbuf *s = e->s;
    union sockaddr_in4in6 addr;
    int r;

    assert(!e->unlinked);

    if (port < 1)
        fatal(1, "invalid port %ld", port);
    inaddr_from_text(&addr, saddr, port, 0);

#ifdef MSWIN_OVLPIO
    struct overlapped *u;

    if (!e->s->overlapped_connect)
        e->s->overlapped_connect = overlapped_alloc(e);
    u = e->s->overlapped_connect;
    assert(u->ref == 1);
    memset (&u->w, '\0', sizeof (u->w));
    u->ref++;
    u->u.uc.connect_addr = addr;
/*
    r = ConnectEx (s->s, (struct sockaddr *) &u->u.uc.connect_addr, sizeof(u->u.uc.connect_addr), NULL, 0, NULL, &u->w.overlapped);
*/
    r = cfn (s->s, &u->u.uc.connect_addr.sa, inaddr_len (&u->u.uc.connect_addr), NULL, 0, NULL, &u->w.overlapped);
    if (!r)
        u->w.err = WSAGetLastError ();
    if (r || (!r && u->w.err != ERROR_IO_PENDING)) {
        assert(!u->completed_next);
        u->completed_next = s->c->o->completed_list;
        s->c->o->completed_list = u;
    }
    s->connecting = 1;
#else
    if (s->bufrd->reading) {
        FD_CLR(s->s, &s->c->o->rd);
        s->bufrd->reading = 0;
    }
    r = connect(s->s, &addr.sa, inaddr_len (&addr));
    if (!(r != 0 && RETRY)) {
        if (s->bufwr->writing && s->connecting)
            FD_CLR(s->s, &s->c->o->wr);
        s->bufwr->writing = s->connecting = 0;
        return 1;   /* WAIT_CONNECT macro passes error to application */
    }
    if (!s->bufwr->writing && !s->connecting)
        FD_SET(s->s, &s->c->o->wr);
    s->bufwr->writing = 0;
    s->connecting = 1;
    corout_update_max_fd(s->c->o, s, 1);
#endif
    return 0;
}

int corout_accept(struct socket *e)
{
    struct sockbuf *s = e->s;

    assert(!e->unlinked);

#ifdef MSWIN_OVLPIO
    struct overlapped *u;
    long err;
    int r;

    assert(!s->accepting);

    if (!e->s->family) {
        union sockaddr_in4in6 a;
        int al = sizeof(a);
        a.sa.sa_family = 0;
        getsockname(e->s->s, &a.sa, &al);
        e->s->family = a.sa.sa_family;
    }
    if (!e->s->overlapped_accept) {
        e->s->overlapped_accept = overlapped_alloc(e);
        e->s->overlapped_accept->u.ua.accept_sock = OS_SOCK_ERR;
    }
    u = e->s->overlapped_accept;
    assert(u->ref == 1);
    memset (&u->w, '\0', sizeof (u->w));
    u->ref++;
    assert(u->u.ua.accept_sock == OS_SOCK_ERR);

/* at about 213k connections, Windows does this:
      accept new sock failed: 10055=>[unknown error] 10055=>[An operation on a socket could not be
      performed because the system lacked sufficient buffer space or because a queue was full.]     */
    if ((u->u.ua.accept_sock = WSASocket(e->s->family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED)) == OS_SOCK_ERR)
        fatal(0, "accept new sock failed");

    int sz = s->c->o->sock_buf_size;
    int yes = 1;
    setsockopt(u->u.ua.accept_sock, SOL_SOCKET, SO_RCVBUF, (char*) &sz, sizeof(sz));
    setsockopt(u->u.ua.accept_sock, SOL_SOCKET, SO_SNDBUF, (char*) &sz, sizeof(sz));
    setsockopt(u->u.ua.accept_sock, IPPROTO_TCP, TCP_NODELAY, (char*) &yes, sizeof(yes));

    if (!(s->c->o->iocp_handle = CreateIoCompletionPort ((HANDLE) u->u.ua.accept_sock, s->c->o->iocp_handle, (ULONG_PTR) NULL, 0)))
        fatal(0, "CreateIoCompletionPort failed");
    r = AcceptEx (s->s, u->u.ua.accept_sock, &u->u.ua.accept_buf, 0, sizeof(u->u.ua.accept_buf) / 2, sizeof(u->u.ua.accept_buf) / 2, NULL, &u->w.overlapped);
    if (!r && ERROR_IO_PENDING != (err = WSAGetLastError ())) {
        u->ref--;
        fprintf(stderr, "AcceptEx failed: %ld\n", err);
        return 1;
    }
    s->accepting = 1;
#else
    if (!s->bufrd->reading && !s->accepting)
        FD_SET(s->s, &s->c->o->rd);
    if (s->bufwr->writing) {
        FD_CLR(s->s, &s->c->o->wr);
        s->bufwr->writing = 0;
    }
    s->bufrd->reading = 0;
    s->accepting = 1;
    corout_update_max_fd(s->c->o, s, 1);
#endif
    return 0;
}

void corout_clear(struct socket *e)
{
#ifdef MSWIN_OVLPIO
    (void) e;
#else
    struct sockbuf *s = e->s;
    if (s->bufrd->reading || s->accepting) {
        FD_CLR(s->s, &s->c->o->rd);
        s->bufrd->reading = s->accepting = 0;
    }
    if (s->bufwr->writing || s->connecting) {
        FD_CLR(s->s, &s->c->o->wr);
        s->bufwr->writing = s->connecting = 0;
    }
    corout_update_max_fd(s->c->o, s, 0);
#endif
}

int corout_connect_success(struct socket *e)
{
    char c;
    if (!e->s)
        return EINVAL;
#ifdef _WIN32
    else if (recv(e->s->s, &c, 0, 0) == -1 && !RETRY)
        return WSAGetLastError() == WSAENOTCONN ? WSAECONNREFUSED : WSAGetLastError();
#else  /* _WIN32 */
    else if (recv(e->s->s, &c, 0, 0) == -1 && !RETRY)
        return errno;
#endif  /* !_WIN32 */
    return 0;
}

int corout_is_writing(struct socket *e)
{
    return e->s && e->s->bufwr->writing;
}

static void corout_socket_free_(struct corout_item *c, struct sockbuf *s)
{
    int i;
    for (i = 0; i < c->n_s; i++)
        if (c->s[i] == s) {
            corout_socket_free__(s);
            c->s[i] = NULL;
            break;
        }
}

void corout_socket_free(struct socket *e)
{
    if (e->s && !e->s->c) {
        assert(e->unlinked == 1);
        corout_socket_free__(e->s);
    } else if (e->s) {
        assert(e->unlinked == 0);
        corout_socket_free_(e->s->c, e->s);
    }
    assert(e->s == NULL);
    free(e);
}

int corout_socket_count(struct corout_item *c)
{
    int i, n;
    for (n = i = 0; i < c->n_s; i++)
        if (c->s[i])
            n++;
    return n;
}

struct buffer *corout_buffer_alloc(const int n)
{
    struct buffer *p;
    p = (struct buffer *) malloc(sizeof(struct buffer));
    memset(p, '\0', sizeof(struct buffer));
    p->ref = 1;
    p->data = (char *) malloc(n);
    p->alloced = n;
    return p;
}

void corout_buffer_free(struct buffer *p)
{
    if (--p->ref == 0) {
        free(p->data);
        free(p);
    }
}

void corout_socket_link(struct corout_item *c, struct socket *e)
{
    struct sockbuf *p = e->s;
    int i, inserted = 0;

    assert (e);
    assert (e->unlinked == 1);
    assert (p);
    assert (!p->c);
    for (i = 0; i < c->n_s; i++) {
        assert (c->s[i] != p);
        if (!c->s[i]) {
            c->s[i] = p;
            inserted = 1;
        }
    }
    if (!inserted) {
        assert (c->n_s < N_SOCKET);
        c->s[c->n_s++] = p;
    }
    p->c = c;
    e->unlinked = 0;
}

static struct socket *corout_socket_alloc(socket_t s)
{
    struct socket *e;
    struct sockbuf *p;

    e = (struct socket *) malloc(sizeof(*e));
    memset(e, '\0', sizeof(*e));
    p = (struct sockbuf *) malloc(sizeof(*p));
    memset(p, '\0', sizeof(*p));
    p->e = e, p->s = s;
    p->bufrd = corout_buffer_alloc(COROUT_BUFFER_ALLOCED);
    p->bufwr = corout_buffer_alloc(COROUT_BUFFER_ALLOCED);
    e->unlinked = 1;
    e->s = p;
    return e;
}

#if 0
static void dump_stack(const char *msg, struct corout_item *c)
{
    int i;
    printf("%s: ", msg);
    for (i = 0; i < COROUT_MAX_STACK_DEPTH; i++)
        printf("%d ", c->stack[i].line_no);
    printf("\n");
}
#endif

#ifndef MSWIN_OVLPIO
static int do_read(struct sockbuf *s)
{
    struct corout_item *c = s->c;
    int r;
    assert (s->bufrd->avail < s->bufrd->alloced);
    r = (int) recv(s->s, s->bufrd->data + s->bufrd->avail,
                   s->bufrd->alloced - s->bufrd->avail, 0);
    if (r < 0 && RETRY) {
        /* do nothing */
        return 1;
    } else if (r <= 0) {
        corout_socket_free_(s->c, s);
        corout_step(c, NULL, CO_EV_RD, NULL);
        return 1;
    } else {
        s->bufrd->avail += r;
        corout_step(c, s, CO_EV_RD, NULL);
    }
    return 0;
}

static int do_write(struct sockbuf *s)
{
    struct corout_item *c = s->c;
    int r;
    r = (int) send(s->s, s->bufwr->data + s->bufwr->written,
                   s->bufwr->avail - s->bufwr->written, 0);
    if (r < 0 && RETRY) {
        /* do nothing */
        return 1;
    } else if (r <= 0) {
        corout_socket_free_(s->c, s);
        corout_step(c, NULL, CO_EV_WR, NULL);
        return 1;
    } else {
        s->bufwr->written += r;
        if (s->bufwr->written == s->bufwr->avail) {
            s->bufwr->written = s->bufwr->avail = 0;
            corout_step(c, s, CO_EV_WR, NULL);
        }
    }
    return 0;
}

static void coroute_step_accept(struct sockbuf *s)
{
    int sz;
    socket_t client;
    struct accept_sock as;
    memset(&as, '\0', sizeof(as));
    as.addrlen = sizeof(as.addr);
    client = accept(s->s, &as.addr.sa, &as.addrlen);
    sz = s->c->o->sock_buf_size;
    setsockopt(s->s, SOL_SOCKET, SO_RCVBUF, (void *) &sz, sizeof(sz));
    setsockopt(s->s, SOL_SOCKET, SO_SNDBUF, (void *) &sz, sizeof(sz));

#ifdef _WIN32
    /* no max */
#else  /* _WIN32 */
    if (client >= (socket_t) FD_SETSIZE - 2) {
        shutdown(client, 2);
        closesocket(client);
        fprintf(stderr, "too many sockets\n");
        return;
    }
#endif  /* !_WIN32 */
    if (client == (socket_t) - 1)    /* normal behavior occasionally on Unix */
        return;
    as.new_client = corout_socket_alloc(client);
    corout_step(s->c, s, CO_EV_ACCEPT, &as);
}
#endif

static int coroute_check_timers(struct corout *o, long long *t_, int *done)
{
    struct corout_item *p;
    long long t;

    t = millitime();
    for (;;) {
        p = (struct corout_item *) pairheap_extremum(o->h);
        if (!p || p->timer > t)
            break;
        pairheap_remove_extremum(o->h);
        p->timer = 0;
        corout_step(p, NULL, CO_EV_TIMER, NULL);
    }
    p = (struct corout_item *) pairheap_extremum(o->h);
#ifdef MSWIN_OVLPIO
    if (!ranktree_count(o->u)) {
        fprintf(stderr, "no more jobs\n");
        *done = 1;
        return 0;
    }
#else
    if (!p && o->max_fd == OS_SOCK_ERR) {
        fprintf(stderr, "no more jobs\n");
        *done = 1;
        return 0;
    }
    if (p && o->max_fd == OS_SOCK_ERR) {
        t = p->timer - t;
        assert(t > 0);
        millisleep(t);
        return 0;
    }
#endif
    t = p ? p->timer - t : 500;
    if (t > 500)
        t = 500;    /* sleep at most 0.5sec for MSWin, in case MSWin ignores ^C */
    *t_ = t;
    return 1;
}

#ifdef USE_OS_POLLING

#define OS_POLLER_EV_RD         1
#define OS_POLLER_EV_WR         2
struct os_poller {
    socket_t fd;
    int evrd, evwr, add;
};

#define N_OS_POLLERS    128

#ifdef __linux__
static void send_os_poll_changes(struct corout *o, struct os_poller *p, const int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int r;
        struct epoll_event e;
        e.data.u64 = p[i].fd;
        e.events = 0;
        if (p[i].evrd)
            e.events |= EPOLLIN;
        if (p[i].evwr)
            e.events |= EPOLLOUT;
        r = epoll_ctl(o->epoll_fd, (p[i].evrd | p[i].evwr) ? (p[i].add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD) : EPOLL_CTL_DEL, p[i].fd, &e);
        assert (!r);
    }
}
#elif defined(__sun)
static void send_os_poll_changes(struct corout *o, struct os_poller *p, const int n)
{
    int i, j = 0;
    struct pollfd q[N_OS_POLLERS * 2];
    for (i = 0; i < n; i++) {
        int e = 0;
        if (p[i].evrd)
            e |= POLLIN;
        if (p[i].evwr)
            e |= POLLOUT;
        if (e && p[i].add) {
            q[j].fd = p[i].fd, q[j].revents = 0, q[j].events = e;
            j++;
        } else if (e) {
            q[j].fd = p[i].fd, q[j].revents = 0, q[j].events = POLLREMOVE;
            j++;
            q[j].fd = p[i].fd, q[j].revents = 0, q[j].events = e;
            j++;
        } else {
            q[j].fd = p[i].fd, q[j].revents = 0, q[j].events = POLLREMOVE;
            j++;
        }
    }
    int r;
    r = write (o->devpoll_fd, q, j * sizeof(struct pollfd));
    assert (r == (int) (j * sizeof(struct pollfd)));
}
#else
#error
#endif

static void remove_os_poll(struct corout *o, socket_t fd)
{
    if (FD_ISSET(fd, &o->old_rd) || FD_ISSET(fd, &o->old_wr)) {
        struct os_poller p;
        p.fd = fd;
        p.evwr = 0;
        p.evrd = 0;
        p.add = 0;
        send_os_poll_changes(o, &p, 1);
    }
    FD_CLR(fd, &o->old_rd);
    FD_CLR(fd, &o->old_wr);
}

#define MAX(a,b)        ((a) > (b) ? (a) : (b))

static void corout_once_os(struct corout *o, const long long t)
{
    unsigned int n = 0, g, i, j, k, max_fd;
    struct os_poller p[N_OS_POLLERS];

    max_fd = MAX(o->max_fd, o->old_max_fd);
    for (g = 0; g < max_fd + 1; g += BIG_FD_SET_CHUNK_BITS) {
        if (o->old_rd.setops[g / BIG_FD_SET_CHUNK_BITS] == o->rd.setops[g / BIG_FD_SET_CHUNK_BITS] &&
            o->old_rd.clrops[g / BIG_FD_SET_CHUNK_BITS] == o->rd.clrops[g / BIG_FD_SET_CHUNK_BITS] &&
            o->old_wr.setops[g / BIG_FD_SET_CHUNK_BITS] == o->wr.setops[g / BIG_FD_SET_CHUNK_BITS] &&
            o->old_wr.clrops[g / BIG_FD_SET_CHUNK_BITS] == o->wr.clrops[g / BIG_FD_SET_CHUNK_BITS])
            continue;
        o->old_rd.setops[g / BIG_FD_SET_CHUNK_BITS] = o->rd.setops[g / BIG_FD_SET_CHUNK_BITS];
        o->old_rd.clrops[g / BIG_FD_SET_CHUNK_BITS] = o->rd.clrops[g / BIG_FD_SET_CHUNK_BITS];
        o->old_wr.setops[g / BIG_FD_SET_CHUNK_BITS] = o->wr.setops[g / BIG_FD_SET_CHUNK_BITS];
        o->old_wr.clrops[g / BIG_FD_SET_CHUNK_BITS] = o->wr.clrops[g / BIG_FD_SET_CHUNK_BITS];
        for (j = g; j < max_fd + 1 && j < g + BIG_FD_SET_CHUNK_BITS; j += BIG_FD_SET_WORD_BITS) {
            size_t old, oldrd, newrd, oldwr, newwr, v;
            oldrd = o->old_rd.d[j / BIG_FD_SET_WORD_BITS];
            oldwr = o->old_wr.d[j / BIG_FD_SET_WORD_BITS];
            newrd = o->rd.d[j / BIG_FD_SET_WORD_BITS];
            newwr = o->wr.d[j / BIG_FD_SET_WORD_BITS];
            if (!(v = (oldrd ^ newrd) | (oldwr ^ newwr)))   /* compare 64-bits at once */
                continue;
            o->old_rd.d[j / BIG_FD_SET_WORD_BITS] = newrd;
            o->old_wr.d[j / BIG_FD_SET_WORD_BITS] = newwr;
            i = __builtin_ctzll(v);         /* start at the first non-matching bit */
            k = 63 - __builtin_clzll(v);    /* finish at the last non-matching bit */
            old = oldrd | oldwr;
            old >>= i, newrd >>= i, newwr >>= i, v >>= i;
            for (; i <= k; i++, old >>= 1, newrd >>= 1, newwr >>= 1, v >>= 1) {
                if (!(v & 1))  /* no changes */
                    continue;
                p[n].fd = (socket_t) i + j;
                p[n].evrd = (newrd & 1);
                p[n].evwr = (newwr & 1);
                p[n].add = !(old & 1);  /* if it wasn't set at all, then we need an "Add" instead of a "Modify" */
                if (++n == N_OS_POLLERS) {
                    send_os_poll_changes(o, p, n);
                    n = 0;
                }
            }
        }
    }
    if (n)
        send_os_poll_changes(o, p, n);
    o->old_max_fd = o->max_fd;

    int r;
#ifdef __linux__
    struct epoll_event events[1024];
    r = epoll_wait(o->epoll_fd, events, 1024, t);
    if (!r)
        return;
    if (r == -1 && RETRY)
        return;
    if (r == -1)
        fatal(0, "epoll_wait");
#elif defined(__sun)
    struct pollfd pollfd[1024];
    struct dvpoll dvpoll;
    memset (&dvpoll, '\0', sizeof (dvpoll));
    dvpoll.dp_fds = pollfd;
    dvpoll.dp_nfds = 1024;
    dvpoll.dp_timeout = (int) t;
    r = ioctl (o->devpoll_fd, DP_POLL, &dvpoll);
    if (!r)
        return;
    if (r == -1 && RETRY)
        return;
    if (r == -1)
        fatal(0, "ioctl(/dev/poll)");
#else
#error
#endif

    for (i = 0; i < (unsigned int) r; i++) {
        struct sockbuf *s;
        struct sockbuf search;
#ifdef __linux__
        search.s = events[i].data.u64;
#elif defined(__sun)
        search.s = pollfd[i].fd;
#else
#error
#endif
        s = (struct sockbuf *) ranktree_find(o->l, &search);
        if (!s || !s->e || !s->e->s)
            continue;
        if ((s->bufrd->reading || s->accepting) && 
#ifdef __linux__
                events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)
#elif defined(__sun)
                pollfd[i].events & (POLLIN | POLLERR | POLLHUP)
#else
#error
#endif
            ) {
            if (s->accepting) {
                coroute_step_accept (s);
                continue;
            }
            if (do_read(s))
                continue;
            /* do_read may have free'd s, so; */
            s = (struct sockbuf *) ranktree_find(o->l, &search);
            if (!s || !s->e || !s->e->s)
                continue;
        }
        if ((s->bufwr->writing || s->connecting) && 
#ifdef __linux__
                events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)
#elif defined(__sun)
                pollfd[i].events & (POLLOUT | POLLERR | POLLHUP)
#else
#error
#endif
            ) {
            if (s->connecting) {
                corout_clear(s->e);
                corout_step(s->c, s, CO_EV_CONNECT, NULL);
                continue;
            }
            if (do_write(s))
                continue;
        }
    }
}

#elif defined(MSWIN_OVLPIO)

void process_overlapped(struct overlapped *u, const int l)
{
    struct sockbuf *s;
    struct corout_item *c;

    assert(u->magic == OVERLAPPED_MAGIC);
    if (!u->e) {  /* indicates call to corout_socket_free, corout_socket_free_, corout_socket_free__, or corout_item_remove */
        overlapped_deref(u, 1);
        return;
    }
    s = u->e->s;
    if (!s) {
        overlapped_deref(u, 1);
        return;
    }
    c = s->c;

    if (overlapped_deref(u, 0)) {
        fprintf(stderr, "dangling overlap u=%p: acc=%p con=%p rcv=%p snd=%p dis=%p\n",
                u, s->overlapped_accept, s->overlapped_connect,
                s->overlapped_recv, s->overlapped_send,
                s->overlapped_disconnect);
        return;
    }
    assert(u->e);
    assert(s);

    if (s->overlapped_accept == u) {
        struct accept_sock as;
        struct sockaddr *local = NULL, *remote = NULL;
        int nlocal = 0, nremote = 0;
        if (u->u.ua.accept_sock == OS_SOCK_ERR)
            return;
        memset (&as, '\0', sizeof(as));
        as.new_client = corout_socket_alloc(u->u.ua.accept_sock);
        GetAcceptExSockaddrs(&u->u.ua.accept_buf, 0, sizeof(as.addr), sizeof(as.addr), &local, &nlocal, &remote, &nremote);
        memcpy(&as.addr, remote, nremote);
        u->u.ua.accept_sock = OS_SOCK_ERR;
        s->accepting = 0;
        corout_step(c, s, CO_EV_ACCEPT, &as);
    } else if (s->overlapped_connect == u) {
        /* according to Windows, this is supposed to allow getpeername to work: */
        setsockopt(s->s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
        corout_step(c, s, CO_EV_CONNECT, NULL);
    } else if (s->overlapped_recv == u) {
        if (u->refbuf) {
            corout_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        if ((int) l <= 0) {
            corout_socket_free_(c, s);
            corout_step(c, NULL, CO_EV_RD, NULL);
            return;
        }
        s->bufrd->avail += l;
        s->bufrd->io_ops++;
        s->bufrd->reading = 0;
        corout_step(c, s, CO_EV_RD, NULL);
    } else if (s->overlapped_send == u) {
        if (u->refbuf) {
            corout_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        if ((int) l <= 0) {
            corout_socket_free_(c, s);
            corout_step(c, NULL, CO_EV_WR, NULL);
            return;
        }
        s->bufwr->written += l;
        s->bufwr->io_ops++;
        assert(s->bufwr->written <= s->bufwr->avail);
        if (!s->bufwr->reading)  /* don't make changes while an overlapped operation is in progress */
            if (s->bufwr->written == s->bufwr->avail)
                s->bufwr->written = s->bufwr->avail = 0;
        s->bufwr->writing = 0;
        corout_step(c, s, CO_EV_WR, NULL);
    } else if (s->overlapped_disconnect == u) {
        corout_socket_free_(c, s);
    } else {
        assert(!"unknown overlapped");
    }
    return;
}

WINBASEAPI WINBOOL WINAPI GetQueuedCompletionStatusEx (HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, WINBOOL fAlertable);

static int corout_once_overlapped(struct corout *o, const long long t)
{
    int r;
    struct overlapped *u, *next;
    u = o->completed_list;
    o->completed_list = NULL;
    for (; u; u = next) {
        next = u->completed_next;
        u->completed_next = NULL;
        process_overlapped(u, u->w.l);
    }

    OVERLAPPED_ENTRY a[1024];
    unsigned long l = 0;

    unsigned int j;
    for (j = 0; j < 1024; j += 4) {
/* if we don't zero these members, we get an unitialized-access warning with DrMemory */
        a[j + 0].lpOverlapped = NULL;
        a[j + 0].dwNumberOfBytesTransferred = 0;
        a[j + 1].lpOverlapped = NULL;
        a[j + 1].dwNumberOfBytesTransferred = 0;
        a[j + 2].lpOverlapped = NULL;
        a[j + 2].dwNumberOfBytesTransferred = 0;
        a[j + 3].lpOverlapped = NULL;
        a[j + 3].dwNumberOfBytesTransferred = 0;
    }

    r = GetQueuedCompletionStatusEx(o->iocp_handle, a, sizeof(a) / sizeof(a[0]), &l, t, 0);
    if (!r && GetLastError() != ERROR_ABANDONED_WAIT_0 && GetLastError() != WAIT_TIMEOUT)
        fatal(0, "GetQueuedCompletionStatusEx failed");
    if (!r)
        l = 0;

    {
        int woke = 0;
        for (j = 0; j < l; j++) {
            if (!a[j].lpOverlapped) {   /* cross-thread wakeup sentinel */
                woke = 1;
                continue;
            }
            process_overlapped((struct overlapped *) a[j].lpOverlapped,
                               a[j].dwNumberOfBytesTransferred);
        }
        if (woke) {
            struct corout_item *p = (struct corout_item *) pairheap_extremum(o->h);
            if (p && p->wakeable) {
                pairheap_remove_extremum(o->h);
                p->timer = 0;
                p->wakeable = 0;
                corout_step(p, NULL, CO_EV_TIMER, NULL);
            }
        }
    }
    return l;
}

#else

static void corout_once_select(struct corout *o, struct ranktree_iterator *i, const long long t)
{
    int r;
    struct sockbuf *s;
    fd_set rd, wr;
    struct timeval tv;

    assert(t > 0);
    assert(o->max_fd != OS_SOCK_ERR);
    tv.tv_sec = (t / 1000);
    tv.tv_usec = (t % 1000) * 1000;
    rd = o->rd, wr = o->wr;
    r = select(o->max_fd + 1, &rd, &wr, NULL, &tv);
    if (!r)
        return;
    if (r == -1 && RETRY)
        return;
    if (r == -1)
        fatal(0, "select");

#ifdef MSWIN_FDSET
/* Windows FD_ISSET is O(n), so jump into the structure and hack: */
    (void) i;
    unsigned int j;
    for(j = 0; j < rd.fd_count; j++) {
        struct sockbuf search;
        search.s = rd.fd_array[j];
        s = (struct sockbuf *) ranktree_find(o->l, &search);
        if (!s)
            continue;
        if (s->accepting) {
            coroute_step_accept (s);
            continue;
        }
        if (do_read(s))
            continue;
    }
    for(j = 0; j < wr.fd_count; j++) {
        struct sockbuf search;
        search.s = wr.fd_array[j];
        s = (struct sockbuf *) ranktree_find(o->l, &search);
        if (!s)
            continue;
        if (s->connecting) {
            corout_clear(s->e);
            corout_step(s->c, s, CO_EV_CONNECT, NULL);
            continue;
        }
        if (do_write(s))
            continue;
    }
#elif defined(__NFDBITS) && defined(__FDS_BITS) && defined(__FD_ELT) && defined(__FD_SETSIZE)
    (void) i;
    long j;
    assert(sizeof (fd_set) * 8 == __FD_SETSIZE);
    for (j = 0; j < o->max_fd + 1; j++) {
        if (!(__FDS_BITS (&rd)[__FD_ELT (j)])) {        /* check 64-bits at once */
            j = __FD_ELT (j) + 1, j *= __NFDBITS, j--;
            continue;
        }
        if (!(__FDS_BITS (&rd)[__FD_ELT (j)] & __FD_MASK (j)))
            continue;
        struct sockbuf search;
        search.s = (socket_t) j;
        s = (struct sockbuf *) ranktree_find(o->l, &search);
        if (!s)
            continue;
        if (s->accepting) {
            coroute_step_accept (s);
            continue;
        }
        if (do_read(s))
            continue;
    }
    for (j = 0; j < o->max_fd + 1; j++) {
        if (!(__FDS_BITS (&wr)[__FD_ELT (j)])) {        /* check 64-bits at once */
            j = __FD_ELT (j) + 1, j *= __NFDBITS, j--;
            continue;
        }
        if (!(__FDS_BITS (&wr)[__FD_ELT (j)] & __FD_MASK (j)))
            continue;
        struct sockbuf search;
        search.s = (socket_t) j;
        s = (struct sockbuf *) ranktree_find(o->l, &search);
        if (!s)
            continue;
        if (s->connecting) {
            corout_clear(s->e);
            corout_step(s->c, s, CO_EV_CONNECT, NULL);
            continue;
        }
        if (do_write(s))
            continue;
    }
#else
    for (s = (struct sockbuf *) ranktree_iterator_first(i, NULL);
         s; s = (struct sockbuf *) ranktree_iterator_next(i)) {
        if (s->accepting && FD_ISSET(s->s, &rd)) {
            coroute_step_accept (s);
            continue;
        }
        if (s->connecting && FD_ISSET(s->s, &wr)) {
            corout_clear(s->e);
            corout_step(s->c, s, CO_EV_CONNECT, NULL);
            continue;
        }
        if (FD_ISSET(s->s, &rd))
            if (do_read(s))
                continue;
        if (FD_ISSET(s->s, &wr))
            if (do_write(s))
                continue;
    }
#endif
}

#endif



void int_handler(const int x)
{
    (void) x;
    got_SIGINT = 1;
}

void corout_run(struct corout *o)
{
    int done = 0;
#if !defined(MSWIN_OVLPIO) && !defined(USE_OS_POLLING)
    struct ranktree_iterator *i;
    i = ranktree_iterator_alloc(o->l);
#endif
    signal(SIGINT, int_handler);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

#ifdef USE_OS_POLLING
    printf("running coroutines, os-style\n");
#elif defined(MSWIN_OVLPIO)
#ifdef MINGWIN64
    printf("running coroutines, overlapped-io, 64-bit\n");
#else
    printf("running coroutines, overlapped-io, 32-bit\n");
#endif
#else
    printf("running coroutines, select\n");
#endif

    while (!got_SIGINT && !done) {
        long long t;
        if (coroute_check_timers(o, &t, &done)) {
#ifdef USE_OS_POLLING
            corout_once_os(o, t);
#elif defined(MSWIN_OVLPIO)
            corout_once_overlapped(o, t);
#else
            corout_once_select(o, i, t);
#endif
        }
    }
#if !defined(MSWIN_OVLPIO) && !defined(USE_OS_POLLING)
    ranktree_iterator_free(i);
#endif
    printf("exit\n");
}

int corout_no_delay(struct socket *e)
{
    int yes = 1;
    return setsockopt(e->s->s, IPPROTO_TCP, TCP_NODELAY, (char *) &yes, sizeof (yes));
}

static socket_t corout_create_socket_(struct corout *o, const long local_port, const char *local_addr, const int server, const int family)
{
    int sz;
    socket_t s = OS_SOCK_ERR;
    const sockoptparam_t yes = 1;
    ioctlopt_t opt = 1;
    union sockaddr_in4in6 addr;

    memset(&addr, '\0', sizeof(addr));
    inaddr_from_text(&addr, local_addr, local_port, family);

#ifdef MSWIN_OVLPIO
    if ((s = WSASocket(addr.sa.sa_family, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED)) == OS_SOCK_ERR) {
        perror("unable to create socket");
        goto err;
    }
    if (!(o->iocp_handle = CreateIoCompletionPort ((HANDLE) s, o->iocp_handle, (ULONG_PTR) NULL, 0))) {
        fprintf(stderr, "CreateIoCompletionPort failed\n");
        goto err;
    }
#else
    (void) o;
    if ((s = socket(addr.sa.sa_family, SOCK_STREAM, 0)) == OS_SOCK_ERR) {
        perror("unable to create socket");
        goto err;
    }
#endif

#ifdef _WIN32
    /* no max */
#else  /* _WIN32 */
    if (s >= (socket_t) FD_SETSIZE - 2) {
        fprintf(stderr, "too many sockets\n");
        goto err;
    }
#endif  /* !_WIN32 */

    ioctl(s, FIONBIO, &opt);

    sz = o->sock_buf_size;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (void *) &sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (void *) &sz, sizeof(sz));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (void *) &yes, sizeof(yes));

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *) &yes,
        sizeof(yes)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed");

#ifndef _WIN32
    if (local_port <= 0 && !local_addr) {
        /* Only Windows needs a bind(0, 0.0.0.0) */
    } else
#endif  /* !_WIN32 */
    if (bind(s, &addr.sa, inaddr_len (&addr)) == -1) {
        fprintf(stderr, "%s:%ld: ", local_addr, local_port);
        perror("bind failed");
        goto err;
    }

    if (server) {
        if (listen(s, 4096) == -1) {
            fprintf(stderr, "%s:%ld: ", local_addr, local_port);
            perror("listen failed");
            goto err;
        }
    }

    return s;

  err:
    if (s != OS_SOCK_ERR)
        closesocket(s);
    return OS_SOCK_ERR;
}

struct socket *corout_socket_listener_alloc(struct corout *o, const enum corout_socket_type type, const long bind_port, const char *bind_addr)
{
    socket_t s;
    assert (type == COROUT_SOCKET_TYPE_TCP);
    s = corout_create_socket_(o, bind_port, bind_addr, 1, 0);
    if (s == OS_SOCK_ERR)
        return NULL;
    return corout_socket_alloc(s);
}

struct socket *corout_socket_client_alloc(struct corout *o, const enum corout_socket_type type, const long bind_port, const char *bind_addr, const char *remote_addr)
{
    socket_t s;
    int family;
    assert (type == COROUT_SOCKET_TYPE_TCP);
    if (remote_addr && strchr(remote_addr, ':'))
        family = AF_INET6;
    else if (bind_addr && strchr(bind_addr, ':'))
        family = AF_INET6;
    else
        family = AF_INET;
    s = corout_create_socket_(o, bind_port, bind_addr, 0, family);
    if (s == OS_SOCK_ERR)
        return NULL;
    return corout_socket_alloc(s);
}

int corout_socket_local_addr(struct socket *e, union sockaddr_in4in6 *addr)
{
    socklen_t al = sizeof(*addr);

    if (!e || !e->s)
        return -1;
    if (getsockname(e->s->s, &addr->sa, &al) != 0)
        return -1;
    return 0;
}

