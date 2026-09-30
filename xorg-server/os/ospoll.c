/*
 * Copyright © 2016 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <dix-config.h>

#include <stdlib.h>
#include <unistd.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <assert.h>

#ifdef WIN32
#define WIN32POLL       1
#define HAVE_OSPOLL     1
#endif

#include "os/xserver_poll.h"

#ifdef WIN32POLL
#include <stdio.h>
#include <X11/Xwinsock.h>
#include <mswsock.h>
#include <mstcpip.h>
#include <X11/xtrans/Xtrans.h>
#include <X11/xtrans/Xtransint.h>
#include "dixstruct_priv.h"
#include "dix/dix_priv.h"
#include "os/osdep.h"
#include "os.h"
#endif

#include "misc.h"               /* for typedef of pointer */
#include "ospoll.h"
#include "list.h"

#ifdef WIN32POLL
extern void ClientReady(int fd, int xevents, void *data);
#endif

#if !HAVE_OSPOLL && defined(HAVE_POLLSET_CREATE)
#include <sys/pollset.h>
#define POLLSET         1
#define HAVE_OSPOLL     1
#endif

#if !HAVE_OSPOLL && defined(HAVE_PORT_CREATE)
#include <port.h>
#include <poll.h>
#define PORT            1
#define HAVE_OSPOLL     1
#endif

#if !HAVE_OSPOLL && defined(HAVE_EPOLL_CREATE1)
#include <sys/epoll.h>
#define EPOLL           1
#define HAVE_OSPOLL     1
#endif

#if !HAVE_OSPOLL
#include "xserver_poll.h"
#define POLL            1
#define HAVE_OSPOLL     1
#endif

#if POLLSET

// pollset-based implementation (as seen on AIX)
struct ospollfd {
    int                 fd;
    int                 xevents;
    short               revents;
    enum ospoll_trigger trigger;
    void                (*callback)(int fd, int xevents, void *data);
    void                *data;
};

struct ospoll {
    pollset_t           ps;
    struct ospollfd     *fds;
    int                 num;
    int                 size;
};

#endif

#if EPOLL || PORT

/* epoll-based implementation */
struct ospollfd {
    int                 fd;
    int                 xevents;
    enum ospoll_trigger trigger;
    void                (*callback)(int fd, int xevents, void *data);
    void                *data;
    struct xorg_list    deleted;
};

struct ospoll {
    int                 epoll_fd;
    struct ospollfd     **fds;
    int                 num;
    int                 size;
    struct xorg_list    deleted;
};

#endif

#if POLL

/* poll-based implementation */
struct ospollfd {
    short               revents;
    enum ospoll_trigger trigger;
    void                (*callback)(int fd, int revents, void *data);
    void                *data;
};

struct ospoll {
    struct pollfd       *fds;
    struct ospollfd     *osfds;
    int                 num;
    int                 size;
    Bool                changed;
};

#endif

#if WIN32POLL
#ifdef POLL
#error only WIN32POLL or POLL may be defined
#endif

/* overlapped-I/O implementation driven by an I/O completion port */

#define OVERLAPPED_MAGIC        0xf91982f5
#define OSPOLL_BUFFER_SIZE      (128 * 1024)

struct overlapped_ {
    struct _OVERLAPPED overlapped;
    WSABUF d;
    DWORD flags;
    DWORD l;
    long err;
};

struct overlapped_accept {
    char accept_buf[sizeof(struct sockaddr_storage) * 2];
    SOCKET accept_sock;
};

struct overlapped_connect {
    struct sockaddr_storage connect_addr;
};

union overlapped_par {
    struct overlapped_accept ua;
    struct overlapped_connect uc;
};

struct overlapped {
    struct overlapped_ w;
    unsigned int magic;
    int ref;
    SOCKET closable;
    struct sockbuf *s;
    struct buffer *refbuf;
    union overlapped_par u;
    struct overlapped *completed_next;
};

/* DisconnectEx is an extension not declared in mswsock.h; load it once at
 * startup via ospoll_disconnect_init, exactly as winconfig/corout.c does. */
typedef BOOL (WINAPI * ospoll_lp_disconnectex_t) (SOCKET, LPOVERLAPPED, DWORD,
                                                  DWORD);
static ospoll_lp_disconnectex_t ospoll_dcfn;

static void
ospoll_disconnect_init(void)
{
    GUID g = WSAID_DISCONNECTEX;
    DWORD bytes;
    SOCKET probe;

    probe = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                      WSA_FLAG_OVERLAPPED);
    if (probe == INVALID_SOCKET)
        return;
    if (WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &g, sizeof(g),
                 &ospoll_dcfn, sizeof(ospoll_dcfn), &bytes, NULL,
                 NULL) == SOCKET_ERROR) {
        closesocket(probe);
        return;
    }
    closesocket(probe);
}

struct ospollfd {
    int fd;
    char want_read;
    char want_write;
    char want_accept;
    enum ospoll_trigger trigger;
    void (*callback)(int fd, int xevents, void *data);
    void *data;
    struct sockbuf *sockbuf;
};

struct ospoll {
    struct ospollfd *osfds;
    int num;
    int size;
    HANDLE iocp_handle;
    struct overlapped *completed_list;
    int iterator;
};

#endif

/* Binary search for the specified file descriptor
 *
 * Returns position if found
 * Returns -position - 1 if not found
 */

static int
ospoll_find(struct ospoll *ospoll, int fd)
{
    int lo = 0;
    int hi = ospoll->num - 1;

    while (lo <= hi) {
        int m = (lo + hi) >> 1;
#if EPOLL || PORT
        int t = ospoll->fds[m]->fd;
#endif
#if POLL || POLLSET
        int t = ospoll->fds[m].fd;
#endif
#if WIN32POLL
        int t = ospoll->osfds[m].fd;
#endif

        if (t < fd)
            lo = m + 1;
        else if (t > fd)
            hi = m - 1;
        else
            return m;
    }
    return -(lo + 1);
}

#if EPOLL || PORT
static void
ospoll_clean_deleted(struct ospoll *ospoll)
{
    struct ospollfd     *osfd, *tmp;

    xorg_list_for_each_entry_safe(osfd, tmp, &ospoll->deleted, deleted) {
        xorg_list_del(&osfd->deleted);
        free(osfd);
    }
}
#endif

/* Insert an element into an array
 *
 * base: base address of array
 * num:  number of elements in the array before the insert
 * size: size of each element
 * pos:  position to insert at
 */
static inline void
array_insert(void *base, size_t num, size_t size, size_t pos)
{
    char *b = base;

    memmove(b + (pos+1) * size,
            b + pos * size,
            (num - pos) * size);
}

/* Delete an element from an array
 *
 * base: base address of array
 * num:  number of elements in the array before the delete
 * size: size of each element
 * pos:  position to delete from
 */
static inline void
array_delete(void *base, size_t num, size_t size, size_t pos)
{
    char *b = base;

    memmove(b + pos * size, b + (pos + 1) * size,
            (num - pos - 1) * size);
}


struct ospoll *
ospoll_create(void)
{
#if POLLSET
    struct ospoll *ospoll = calloc(1, sizeof (struct ospoll));

    ospoll->ps = pollset_create(-1);
    if (ospoll->ps < 0) {
        free (ospoll);
        return NULL;
    }
    return ospoll;
#endif
#if PORT
    struct ospoll *ospoll = calloc(1, sizeof (struct ospoll));

    ospoll->epoll_fd = port_create();
    if (ospoll->epoll_fd < 0) {
        free (ospoll);
        return NULL;
    }
    xorg_list_init(&ospoll->deleted);
    return ospoll;
#endif
#if EPOLL
    struct ospoll       *ospoll = calloc(1, sizeof (struct ospoll));

    ospoll->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ospoll->epoll_fd < 0) {
        free (ospoll);
        return NULL;
    }
    xorg_list_init(&ospoll->deleted);
    return ospoll;
#endif
#if POLL
    return calloc(1, sizeof (struct ospoll));
#endif
#if WIN32POLL
    struct ospoll *ospoll = calloc(1, sizeof (struct ospoll));

    if (!ospoll)
        return NULL;
    ospoll_disconnect_init();
    ospoll->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL,
                                                 (ULONG_PTR) 0, 0);
    if (!ospoll->iocp_handle) {
        free(ospoll);
        return NULL;
    }
    return ospoll;
#endif
}

void
ospoll_destroy(struct ospoll *ospoll)
{
#if POLLSET
    if (ospoll) {
        assert (ospoll->num == 0);
        pollset_destroy(ospoll->ps);
        free(ospoll->fds);
        free(ospoll);
    }
#endif
#if EPOLL || PORT
    if (ospoll) {
        assert (ospoll->num == 0);
        close(ospoll->epoll_fd);
        ospoll_clean_deleted(ospoll);
        free(ospoll->fds);
        free(ospoll);
    }
#endif
#if POLL
    if (ospoll) {
        assert (ospoll->num == 0);
        free (ospoll->fds);
        free (ospoll->osfds);
        free (ospoll);
    }
#endif
#if WIN32POLL
    if (ospoll) {
        assert (ospoll->num == 0);
        if (ospoll->iocp_handle)
            CloseHandle(ospoll->iocp_handle);
        free (ospoll->osfds);
        free (ospoll);
    }
#endif
}

Bool
ospoll_add(struct ospoll *ospoll, int fd,
           enum ospoll_trigger trigger,
           void (*callback)(int fd, int xevents, void *data),
           void *data)
{
    int pos = ospoll_find(ospoll, fd);
#if POLLSET
    if (pos < 0) {
        if (ospoll->num == ospoll->size) {
            struct ospollfd *new_fds;
            int new_size = ospoll->size ? ospoll->size * 2 : MAXCLIENTS * 2;

            new_fds = reallocarray(ospoll->fds, new_size, sizeof (ospoll->fds[0]));
            if (!new_fds)
                return FALSE;
            ospoll->fds = new_fds;
            ospoll->size = new_size;
        }
        pos = -pos - 1;
        array_insert(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->num++;

        ospoll->fds[pos].fd = fd;
        ospoll->fds[pos].xevents = 0;
        ospoll->fds[pos].revents = 0;
    }
    ospoll->fds[pos].trigger = trigger;
    ospoll->fds[pos].callback = callback;
    ospoll->fds[pos].data = data;
#endif
#if PORT
    struct ospollfd *osfd;

    if (pos < 0) {
        osfd = calloc(1, sizeof (struct ospollfd));
        if (!osfd)
            return FALSE;

        if (ospoll->num >= ospoll->size) {
            struct ospollfd **new_fds;
            int new_size = ospoll->size ? ospoll->size * 2 : MAXCLIENTS * 2;

            new_fds = reallocarray(ospoll->fds, new_size, sizeof (ospoll->fds[0]));
            if (!new_fds) {
                free (osfd);
                return FALSE;
            }
            ospoll->fds = new_fds;
            ospoll->size = new_size;
        }

        osfd->fd = fd;
        osfd->xevents = 0;

        pos = -pos - 1;
        array_insert(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->fds[pos] = osfd;
        ospoll->num++;
    } else {
        osfd = ospoll->fds[pos];
    }
    osfd->data = data;
    osfd->callback = callback;
    osfd->trigger = trigger;
#endif
#if EPOLL
    struct ospollfd *osfd;

    if (pos < 0) {

        struct epoll_event ev;

        osfd = calloc(1, sizeof (struct ospollfd));
        if (!osfd)
            return FALSE;

        if (ospoll->num >= ospoll->size) {
            struct ospollfd **new_fds;
            int new_size = ospoll->size ? ospoll->size * 2 : MAXCLIENTS * 2;

            new_fds = reallocarray(ospoll->fds, new_size, sizeof (ospoll->fds[0]));
            if (!new_fds) {
                free (osfd);
                return FALSE;
            }
            ospoll->fds = new_fds;
            ospoll->size = new_size;
        }

        ev.events = 0;
        ev.data.ptr = osfd;
        if (trigger == ospoll_trigger_edge)
            ev.events |= EPOLLET;
        if (epoll_ctl(ospoll->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
            free(osfd);
            return FALSE;
        }
        osfd->fd = fd;
        osfd->xevents = 0;

        pos = -pos - 1;
        array_insert(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->fds[pos] = osfd;
        ospoll->num++;
    } else {
        osfd = ospoll->fds[pos];
    }
    osfd->data = data;
    osfd->callback = callback;
    osfd->trigger = trigger;
#endif
#if POLL
    if (pos < 0) {
        if (ospoll->num == ospoll->size) {
            struct pollfd   *new_fds;
            struct ospollfd *new_osfds;
            int             new_size = ospoll->size ? ospoll->size * 2 : MAXCLIENTS * 2;

            new_fds = reallocarray(ospoll->fds, new_size, sizeof (ospoll->fds[0]));
            if (!new_fds)
                return FALSE;
            ospoll->fds = new_fds;
            new_osfds = reallocarray(ospoll->osfds, new_size, sizeof (ospoll->osfds[0]));
            if (!new_osfds)
                return FALSE;
            ospoll->osfds = new_osfds;
            ospoll->size = new_size;
        }
        pos = -pos - 1;
        array_insert(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        array_insert(ospoll->osfds, ospoll->num, sizeof (ospoll->osfds[0]), pos);
        ospoll->num++;
        ospoll->changed = TRUE;

        ospoll->fds[pos].fd = fd;
        ospoll->fds[pos].events = 0;
        ospoll->fds[pos].revents = 0;
        ospoll->osfds[pos].revents = 0;
    }
    ospoll->osfds[pos].trigger = trigger;
    ospoll->osfds[pos].callback = callback;
    ospoll->osfds[pos].data = data;
#endif
#if WIN32POLL
    if (pos < 0) {
        if (ospoll->num == ospoll->size) {
            struct ospollfd *new_osfds;
            int             new_size = ospoll->size ? ospoll->size * 2 : MAXCLIENTS * 2;

            new_osfds = reallocarray(ospoll->osfds, new_size, sizeof (ospoll->osfds[0]));
            if (!new_osfds)
                return FALSE;
            ospoll->osfds = new_osfds;
            ospoll->size = new_size;
        }
        pos = -pos - 1;
        array_insert(ospoll->osfds, ospoll->num, sizeof (ospoll->osfds[0]), pos);
        ospoll->num++;
        if (pos <= ospoll->iterator)
            ospoll->iterator++;

        ospoll->osfds[pos].fd = fd;
        ospoll->osfds[pos].want_read = 0;
        ospoll->osfds[pos].want_write = 0;
        ospoll->osfds[pos].want_accept = 0;
        ospoll->osfds[pos].sockbuf = NULL;
    }
    ospoll->osfds[pos].trigger = trigger;
    ospoll->osfds[pos].callback = callback;
    ospoll->osfds[pos].data = data;
#endif
    return TRUE;
}

void
ospoll_remove(struct ospoll *ospoll, int fd)
{
    int pos = ospoll_find(ospoll, fd);

    pos = ospoll_find(ospoll, fd);
    if (pos >= 0) {
#if POLLSET
        struct ospollfd *osfd = &ospoll->fds[pos];
        struct poll_ctl ctl = { .cmd = PS_DELETE, .fd = fd };
        pollset_ctl(ospoll->ps, &ctl, 1);

        array_delete(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->num--;
#endif
#if PORT
        struct ospollfd *osfd = ospoll->fds[pos];
        port_dissociate(ospoll->epoll_fd, PORT_SOURCE_FD, fd);

        array_delete(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->num--;
        osfd->callback = NULL;
        osfd->data = NULL;
        xorg_list_add(&osfd->deleted, &ospoll->deleted);
#endif
#if EPOLL
        struct ospollfd *osfd = ospoll->fds[pos];
        struct epoll_event ev;
        ev.events = 0;
        ev.data.ptr = osfd;
        (void) epoll_ctl(ospoll->epoll_fd, EPOLL_CTL_DEL, fd, &ev);

        array_delete(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        ospoll->num--;
        osfd->callback = NULL;
        osfd->data = NULL;
        xorg_list_add(&osfd->deleted, &ospoll->deleted);
#endif
#if POLL
        array_delete(ospoll->fds, ospoll->num, sizeof (ospoll->fds[0]), pos);
        array_delete(ospoll->osfds, ospoll->num, sizeof (ospoll->osfds[0]), pos);
        ospoll->num--;
        ospoll->changed = TRUE;
#endif
#if WIN32POLL
        array_delete(ospoll->osfds, ospoll->num, sizeof (ospoll->osfds[0]), pos);
        ospoll->num--;
        if (pos <= ospoll->iterator)
            ospoll->iterator--;
#endif
    }
}

#if PORT
static void
epoll_mod(struct ospoll *ospoll, struct ospollfd *osfd)
{
    int events = 0;
    if (osfd->xevents & X_NOTIFY_READ)
        events |= POLLIN;
    if (osfd->xevents & X_NOTIFY_WRITE)
        events |= POLLOUT;
    port_associate(ospoll->epoll_fd, PORT_SOURCE_FD, osfd->fd, events, osfd);
}
#endif

#if EPOLL
static void
epoll_mod(struct ospoll *ospoll, struct ospollfd *osfd)
{
    struct epoll_event ev;
    ev.events = 0;
    if (osfd->xevents & X_NOTIFY_READ)
        ev.events |= EPOLLIN;
    if (osfd->xevents & X_NOTIFY_WRITE)
        ev.events |= EPOLLOUT;
    if (osfd->trigger == ospoll_trigger_edge)
        ev.events |= EPOLLET;
    ev.data.ptr = osfd;
    (void) epoll_ctl(ospoll->epoll_fd, EPOLL_CTL_MOD, osfd->fd, &ev);
}
#endif

void
ospoll_listen(struct ospoll *ospoll, int fd, int xevents)
{
    int pos = ospoll_find(ospoll, fd);

    if (pos >= 0) {
#if POLLSET
        struct poll_ctl ctl = { .cmd = PS_MOD, .fd = fd };
        if (xevents & X_NOTIFY_READ) {
            ctl.events |= POLLIN;
            ospoll->fds[pos].revents &= ~POLLIN;
        }
        if (xevents & X_NOTIFY_WRITE) {
            ctl.events |= POLLOUT;
            ospoll->fds[pos].revents &= ~POLLOUT;
        }
        pollset_ctl(ospoll->ps, &ctl, 1);
        ospoll->fds[pos].xevents |= xevents;
#endif
#if EPOLL || PORT
        struct ospollfd *osfd = ospoll->fds[pos];
        osfd->xevents |= xevents;
        epoll_mod(ospoll, osfd);
#endif
#if POLL
        if (xevents & X_NOTIFY_READ) {
            ospoll->fds[pos].events |= POLLIN;
            ospoll->osfds[pos].revents &= ~POLLIN;
        }
        if (xevents & X_NOTIFY_WRITE) {
            ospoll->fds[pos].events |= POLLOUT;
            ospoll->osfds[pos].revents &= ~POLLOUT;
        }
#endif
#if WIN32POLL
        if (xevents & X_NOTIFY_READ)
            ospoll->osfds[pos].want_read = 1;
        if (xevents & X_NOTIFY_WRITE)
            ospoll->osfds[pos].want_write = 1;
#endif
    }
}

void
ospoll_mute(struct ospoll *ospoll, int fd, int xevents)
{
    int pos = ospoll_find(ospoll, fd);

    if (pos >= 0) {
#if POLLSET
        struct ospollfd *osfd = &ospoll->fds[pos];
        osfd->xevents &= ~xevents;
        struct poll_ctl ctl = { .cmd = PS_DELETE, .fd = fd };
        pollset_ctl(ospoll->ps, &ctl, 1);
        if (osfd->xevents) {
            ctl.cmd = PS_ADD;
            if (osfd->xevents & X_NOTIFY_READ) {
                ctl.events |= POLLIN;
            }
            if (osfd->xevents & X_NOTIFY_WRITE) {
                ctl.events |= POLLOUT;
            }
            pollset_ctl(ospoll->ps, &ctl, 1);
        }
#endif
#if EPOLL || PORT
        struct ospollfd *osfd = ospoll->fds[pos];
        osfd->xevents &= ~xevents;
        epoll_mod(ospoll, osfd);
#endif
#if POLL
        if (xevents & X_NOTIFY_READ)
            ospoll->fds[pos].events &= ~POLLIN;
        if (xevents & X_NOTIFY_WRITE)
            ospoll->fds[pos].events &= ~POLLOUT;
#endif
#if WIN32POLL
        if (xevents & X_NOTIFY_READ)
            ospoll->osfds[pos].want_read = 0;
        if (xevents & X_NOTIFY_WRITE)
            ospoll->osfds[pos].want_write = 0;
#endif
    }
}


#if WIN32POLL

static struct buffer *
ospoll_buffer_alloc(int n)
{
    struct buffer *p = malloc(sizeof(struct buffer));

    if (!p)
        return NULL;
    memset(p, 0, sizeof(struct buffer));
    p->ref = 1;
    p->data = malloc(n);
    if (!p->data) {
        free(p);
        return NULL;
    }
    p->alloced = n;
    return p;
}

static void
ospoll_buffer_free(struct buffer *p)
{
    if (!p)
        return;
    if (--p->ref == 0) {
        free(p->data);
        free(p);
    }
}

static struct overlapped *
ospoll_overlapped_alloc(struct sockbuf *s)
{
    struct overlapped *u = malloc(sizeof(struct overlapped));

    memset(u, 0, sizeof(*u));
    u->magic = OVERLAPPED_MAGIC;
    u->ref = 1;
    u->closable = INVALID_SOCKET;
    u->s = s;
    return u;
}

static int
ospoll_overlapped_deref(struct overlapped *u, int clean)
{
    if (!u)
        return 1;
    assert(u->magic == OVERLAPPED_MAGIC);
    if (clean)
        u->s = NULL;
    assert(u->ref >= 0);
    if (!--u->ref) {
        if (u->closable != INVALID_SOCKET)
            closesocket(u->closable);
        if (u->refbuf) {
            ospoll_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        u->magic = 0;
        free(u);
        return 1;
    }
    return 0;
}

struct sockbuf *
ospoll_sockbuf_alloc(int s)
{
    struct sockbuf *p = calloc(1, sizeof(struct sockbuf));

    if (!p)
        return NULL;
    p->s = s;
    p->bufrd = ospoll_buffer_alloc(OSPOLL_BUFFER_SIZE);
    p->bufwr = ospoll_buffer_alloc(OSPOLL_BUFFER_SIZE);
    if (!p->bufrd || !p->bufwr) {
        ospoll_buffer_free(p->bufrd);
        ospoll_buffer_free(p->bufwr);
        free(p);
        return NULL;
    }
    return p;
}

/* DisconnectEx is an extension not declared in mswsock.h; the pointer is
 * loaded once at startup by ospoll_disconnect_init (see ospoll_create). */
static BOOL
ospoll_disconnect_ex(SOCKET s, LPOVERLAPPED ov, DWORD flags, DWORD reserved)
{
    if (!ospoll_dcfn)
        return FALSE;
    return ospoll_dcfn(s, ov, flags, reserved);
}

/* Initiate a deferred teardown: post DisconnectEx and stash the socket handle
 * in u->closable so it is closed only when the disconnect completion is
 * drained (see ospoll_overlapped_deref). Pending recv/send/accept overlapped
 * are kept alive by refcounts and detached via u->s = NULL in
 * ospoll_sockbuf_free, so no completion ever dereferences the freed sockbuf. */
static void
ospoll_socket_disconnect(struct sockbuf *sb)
{
    struct overlapped *u;
    BOOL r;
    int err;

    if (sb->disconnecting)
        return;
    assert(!sb->overlapped_disconnect);
    sb->overlapped_disconnect = ospoll_overlapped_alloc(sb);
    u = sb->overlapped_disconnect;
    assert(u->ref == 1);
    memset(&u->w, 0, sizeof(u->w));
    u->ref++;
    u->closable = (SOCKET) sb->s;
    r = ospoll_disconnect_ex((SOCKET) sb->s, &u->w.overlapped, 0, 0);
    if (!r && (err = WSAGetLastError()) != ERROR_IO_PENDING) {
        ErrorF("IOCP: DisconnectEx failed fd=%d err=%d\n", sb->s, err);
        u->closable = INVALID_SOCKET;
        closesocket((SOCKET) sb->s);
        u->ref--;
    }
    sb->disconnecting = 1;
}

void
ospoll_sockbuf_free(struct sockbuf *sb)
{
    if (!sb)
        return;
    while (sb->accept_head) {
        struct sockbuf *next = sb->accept_head->accept_next;

        ospoll_sockbuf_free(sb->accept_head);
        sb->accept_head = next;
    }
    if (sb->overlapped_accept) {
        if (sb->overlapped_accept->u.ua.accept_sock != INVALID_SOCKET)
            closesocket(sb->overlapped_accept->u.ua.accept_sock);
        ospoll_overlapped_deref(sb->overlapped_accept, 1);
    }
    ospoll_overlapped_deref(sb->overlapped_send, 1);
    ospoll_overlapped_deref(sb->overlapped_recv, 1);
    ospoll_overlapped_deref(sb->overlapped_connect, 1);
    if (sb->s != INVALID_SOCKET)
        ospoll_socket_disconnect(sb);
    ospoll_overlapped_deref(sb->overlapped_disconnect, 1);
    ospoll_buffer_free(sb->bufrd);
    ospoll_buffer_free(sb->bufwr);
    free(sb);
}

void
ospoll_bind_sockbuf(struct ospoll *ospoll, int fd, struct sockbuf *sockbuf)
{
    int pos = ospoll_find(ospoll, fd);

    if (pos < 0 || !sockbuf)
        return;
    ospoll->osfds[pos].sockbuf = sockbuf;
    if (sockbuf->listener) {
        ospoll->osfds[pos].want_accept = 1;
        ospoll->osfds[pos].want_read = 0;
    }
    if (sockbuf->s != INVALID_SOCKET)
        CreateIoCompletionPort((HANDLE) (SOCKET) sockbuf->s, ospoll->iocp_handle,
                               (ULONG_PTR) 0, 0);
}

static void
ospoll_read(struct ospoll *ospoll, struct sockbuf *s)
{
    struct overlapped *u;
    int r;

    if (s->bufrd->reading)
        return;
    if (s->bufrd->written == s->bufrd->avail)
        s->bufrd->written = s->bufrd->avail = 0;
    if (s->bufrd->avail >= s->bufrd->alloced)
        return;
    if (!s->overlapped_recv)
        s->overlapped_recv = ospoll_overlapped_alloc(s);
    u = s->overlapped_recv;
    assert(u->ref == 1);
    memset(&u->w, 0, sizeof(u->w));
    assert(!u->refbuf);
    u->refbuf = s->bufrd;
    u->refbuf->ref++;
    u->w.d.buf = s->bufrd->data + s->bufrd->avail;
    u->w.d.len = s->bufrd->alloced - s->bufrd->avail;
    assert(u->w.d.len);
    u->ref++;

    if (s->dgram) {
        s->udp_fromlen = sizeof(s->udp_from);
        r = WSARecvFrom(s->s, &u->w.d, 1, &u->w.l, &u->w.flags,
                        (struct sockaddr *) &s->udp_from, &s->udp_fromlen,
                        &u->w.overlapped, NULL);
    } else {
        r = WSARecv(s->s, &u->w.d, 1, &u->w.l, &u->w.flags, &u->w.overlapped,
                    NULL);
    }
    if (r == SOCKET_ERROR)
        u->w.err = WSAGetLastError();
    if (r == SOCKET_ERROR && u->w.err != ERROR_IO_PENDING) {
        assert(!u->completed_next);
        u->completed_next = ospoll->completed_list;
        ospoll->completed_list = u;
    }
    s->bufrd->reading = 1;
}

static void
ospoll_write(struct ospoll *ospoll, struct sockbuf *s)
{
    struct overlapped *u;
    int r;

    if (s->bufwr->writing)
        return;
    if (s->bufwr->written >= s->bufwr->avail)
        return;
    if (!s->overlapped_send)
        s->overlapped_send = ospoll_overlapped_alloc(s);
    u = s->overlapped_send;
    assert(u->ref == 1);
    memset(&u->w, 0, sizeof(u->w));
    assert(!u->refbuf);
    u->refbuf = s->bufwr;
    u->refbuf->ref++;
    u->w.d.buf = s->bufwr->data + s->bufwr->written;
    u->w.d.len = s->bufwr->avail - s->bufwr->written;
    u->ref++;

    r = WSASend(s->s, &u->w.d, 1, &u->w.l, u->w.flags, &u->w.overlapped, NULL);
    if (r == SOCKET_ERROR)
        u->w.err = WSAGetLastError();
    if (r == SOCKET_ERROR && u->w.err != ERROR_IO_PENDING) {
        assert(!u->completed_next);
        u->completed_next = ospoll->completed_list;
        ospoll->completed_list = u;
    }
    s->bufwr->writing = 1;
}

static void
ospoll_accept(struct ospoll *ospoll, struct sockbuf *s)
{
    struct overlapped *u;
    int r, err;

    if (s->accepting)
        return;
    if (!s->family) {
        struct sockaddr_storage a;
        int al = sizeof(a);

        a.ss_family = 0;
        getsockname(s->s, (struct sockaddr *) &a, &al);
        s->family = a.ss_family;
    }
    if (!s->overlapped_accept) {
        s->overlapped_accept = ospoll_overlapped_alloc(s);
        s->overlapped_accept->u.ua.accept_sock = INVALID_SOCKET;
    }
    u = s->overlapped_accept;
    assert(u->ref == 1);
    memset(&u->w, 0, sizeof(u->w));
    u->ref++;
    assert(u->u.ua.accept_sock == INVALID_SOCKET);

    u->u.ua.accept_sock = WSASocket(s->family, SOCK_STREAM, IPPROTO_TCP, NULL, 0,
                                    WSA_FLAG_OVERLAPPED);
    if (u->u.ua.accept_sock == INVALID_SOCKET) {
        u->ref--;
        u->u.ua.accept_sock = INVALID_SOCKET;
        return;
    }
    if (!CreateIoCompletionPort((HANDLE) u->u.ua.accept_sock,
                                ospoll->iocp_handle, (ULONG_PTR) 0, 0)) {
        closesocket(u->u.ua.accept_sock);
        u->u.ua.accept_sock = INVALID_SOCKET;
        u->ref--;
        return;
    }

    r = AcceptEx(s->s, u->u.ua.accept_sock, u->u.ua.accept_buf, 0,
                 sizeof(u->u.ua.accept_buf) / 2,
                 sizeof(u->u.ua.accept_buf) / 2, NULL, &u->w.overlapped);
    if (!r && ERROR_IO_PENDING != (err = WSAGetLastError())) {
        u->ref--;
        ErrorF("IOCP: AcceptEx failed listen=%d accept=%d err=%ld\n",
               s->s, (int) u->u.ua.accept_sock, (long) err);
        return;
    }
    ErrorF("IOCP: AcceptEx posted listen=%d accept=%d\n",
           s->s, (int) u->u.ua.accept_sock);
    s->accepting = 1;
}

static void
process_overlapped(struct ospoll *ospoll, struct overlapped *u, int l)
{
    struct sockbuf *s;

    assert(u->magic == OVERLAPPED_MAGIC);
    if (!u->s) {
        ospoll_overlapped_deref(u, 1);
        return;
    }
    s = u->s;
    if (ospoll_overlapped_deref(u, 0)) {
        fprintf(stderr, "dangling overlap u=%p\n", (void *) u);
        return;
    }
    assert(s);

    if (s->overlapped_accept == u) {
        struct sockaddr *local = NULL, *remote = NULL;
        int nlocal = 0, nremote = 0;
        struct sockbuf *new_sb;

        if (u->u.ua.accept_sock == INVALID_SOCKET)
            return;
        new_sb = ospoll_sockbuf_alloc(u->u.ua.accept_sock);
        ErrorF("IOCP: AcceptEx completed listen=%d accept=%d new_sb=%p\n",
               s->s, (int) u->u.ua.accept_sock, (void *) new_sb);
        u->u.ua.accept_sock = INVALID_SOCKET;
        if (!new_sb)
            return;
        GetAcceptExSockaddrs(u->u.ua.accept_buf, 0,
                             sizeof(struct sockaddr_storage),
                             sizeof(struct sockaddr_storage),
                             &local, &nlocal, &remote, &nremote);
        {
            SOCKET listen_sock = (SOCKET) s->s;
            int sr = setsockopt(new_sb->s, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                (char *) &listen_sock, sizeof(listen_sock));
            ErrorF("IOCP: SO_UPDATE_ACCEPT_CONTEXT on %d (listen %d) -> %d WSA %d\n",
                   new_sb->s, s->s, sr, WSAGetLastError());
        }
        s->accepting = 0;
        new_sb->accept_next = NULL;
        if (!s->accept_head)
            s->accept_head = new_sb;
        else {
            struct sockbuf *tail = s->accept_head;

            while (tail->accept_next)
                tail = tail->accept_next;
            tail->accept_next = new_sb;
        }
    } else if (s->overlapped_connect == u) {
        setsockopt(s->s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
        s->connecting = 0;
    } else if (s->overlapped_recv == u) {
        if (u->refbuf) {
            ospoll_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        if (l <= 0) {
            s->bufrd->avail = 0;
            s->bufrd->written = 0;
            if (s->dgram) {
                s->bufrd->reading = 0;
                return;
            }
            s->overlapped_recv = NULL;
            s->eof = 1;
            return;
        }
        s->bufrd->avail += l;
        s->bufrd->io_ops++;
        s->bufrd->reading = 0;
        ErrorF("IOCP: recv %d bytes on fd %d (avail=%d)\n",
               l, s->s, s->bufrd->avail);
    } else if (s->overlapped_send == u) {
        if (u->refbuf) {
            ospoll_buffer_free(u->refbuf);
            u->refbuf = NULL;
        }
        if (l <= 0) {
            s->bufwr->avail = 0;
            s->bufwr->written = 0;
            s->overlapped_send = NULL;
            s->eof = 1;
            return;
        }
        s->bufwr->written += l;
        s->bufwr->io_ops++;
        assert(s->bufwr->written <= s->bufwr->avail);
        if (!s->bufwr->reading)
            if (s->bufwr->written == s->bufwr->avail)
                s->bufwr->written = s->bufwr->avail = 0;
        s->bufwr->writing = 0;
        ErrorF("IOCP: sent %d bytes on fd %d (written=%d avail=%d)\n",
               l, s->s, s->bufwr->written, s->bufwr->avail);
    } else if (s->overlapped_disconnect == u) {
        /* socket teardown handled by ospoll_sockbuf_free */
    } else {
        assert(!"unknown overlapped");
    }
}

WINBASEAPI WINBOOL WINAPI GetQueuedCompletionStatusEx (HANDLE CompletionPort, LPOVERLAPPED_ENTRY lpCompletionPortEntries, ULONG ulCount, PULONG ulNumEntriesRemoved, DWORD dwMilliseconds, WINBOOL fAlertable);

static void
ospoll_drain(struct ospoll *ospoll, DWORD ms_timeout)
{
    struct overlapped *u, *next;
    OVERLAPPED_ENTRY entries[256];
    ULONG n = 0;
    BOOL ok;
    ULONG j;

    u = ospoll->completed_list;
    ospoll->completed_list = NULL;
    for (; u; u = next) {
        next = u->completed_next;
        u->completed_next = NULL;
        process_overlapped(ospoll, u, (int) u->w.l);
    }

    ok = GetQueuedCompletionStatusEx(ospoll->iocp_handle, entries, 256, &n,
                                     ms_timeout, FALSE);
    if (!ok && GetLastError() != WAIT_TIMEOUT)
        fprintf(stderr, "GetQueuedCompletionStatusEx failed: %ld\n",
                (long) GetLastError());

    for (j = 0; j < n; j++) {
        if (!entries[j].lpOverlapped)
            continue;
        process_overlapped(ospoll,
                           (struct overlapped *) entries[j].lpOverlapped,
                           (int) entries[j].dwNumberOfBytesTransferred);
    }
}

#endif


int
ospoll_wait(struct ospoll *ospoll, int timeout)
{
    int nready;
#if POLLSET
#define MAX_EVENTS      256
    struct pollfd events[MAX_EVENTS];

    nready = pollset_poll(ospoll->ps, events, MAX_EVENTS, timeout);
    for (int i = 0; i < nready; i++) {
        struct pollfd *ev = &events[i];
        int pos = ospoll_find(ospoll, ev->fd);
        struct ospollfd *osfd = &ospoll->fds[pos];
        short revents = ev->revents;
        short oldevents = osfd->revents;

        osfd->revents = (revents & (POLLIN|POLLOUT));
        if (osfd->trigger == ospoll_trigger_edge)
            revents &= ~oldevents;
        if (revents) {
            int xevents = 0;
            if (revents & POLLIN)
                xevents |= X_NOTIFY_READ;
            if (revents & POLLOUT)
                xevents |= X_NOTIFY_WRITE;
            if (revents & (~(POLLIN|POLLOUT)))
                xevents |= X_NOTIFY_ERROR;
            osfd->callback(osfd->fd, xevents, osfd->data);
        }
    }
#endif
#if PORT
#define MAX_EVENTS      256
    port_event_t events[MAX_EVENTS];
    uint_t nget = 1;
    timespec_t port_timeout = {
        .tv_sec = timeout / 1000,
        .tv_nsec = (timeout % 1000) * 1000000
    };

    nready = 0;
    if (port_getn(ospoll->epoll_fd, events, MAX_EVENTS, &nget, &port_timeout)
        == 0) {
        nready = nget;
    }
    for (int i = 0; i < nready; i++) {
        port_event_t *ev = &events[i];
        struct ospollfd *osfd = ev->portev_user;
        uint32_t revents = ev->portev_events;
        int xevents = 0;

        if (revents & POLLIN)
            xevents |= X_NOTIFY_READ;
        if (revents & POLLOUT)
            xevents |= X_NOTIFY_WRITE;
        if (revents & (~(POLLIN|POLLOUT)))
            xevents |= X_NOTIFY_ERROR;

        if (osfd->callback)
            osfd->callback(osfd->fd, xevents, osfd->data);

        if (osfd->trigger == ospoll_trigger_level &&
            !xorg_list_is_empty(&osfd->deleted)) {
            epoll_mod(ospoll, osfd);
        }
    }
    ospoll_clean_deleted(ospoll);
#endif
#if EPOLL
#define MAX_EVENTS      256
    struct epoll_event events[MAX_EVENTS];
    int i;

    nready = epoll_wait(ospoll->epoll_fd, events, MAX_EVENTS, timeout);
    for (i = 0; i < nready; i++) {
        struct epoll_event *ev = &events[i];
        struct ospollfd *osfd = ev->data.ptr;
        uint32_t revents = ev->events;
        int xevents = 0;

        if (revents & EPOLLIN)
            xevents |= X_NOTIFY_READ;
        if (revents & EPOLLOUT)
            xevents |= X_NOTIFY_WRITE;
        if (revents & (~(EPOLLIN|EPOLLOUT)))
            xevents |= X_NOTIFY_ERROR;

        if (osfd->callback)
            osfd->callback(osfd->fd, xevents, osfd->data);
    }
    ospoll_clean_deleted(ospoll);
#endif
#if WIN32POLL
    DWORD start;
    int f;

    start = GetTickCount();
    nready = 0;
    for (;;) {
        /* post outstanding recv/send/accept operations */
        for (f = 0; f < ospoll->num; f++) {
            struct ospollfd *osfd = &ospoll->osfds[f];
            struct sockbuf *s = osfd->sockbuf;

            if (!s)
                continue;
            if (osfd->want_read && !s->bufrd->reading &&
                (s->bufrd->avail < s->bufrd->alloced || s->bufrd->written == s->bufrd->avail))
                ospoll_read(ospoll, s);
            if (!s->bufwr->writing && s->bufwr->avail > s->bufwr->written)
                ospoll_write(ospoll, s);
            if (osfd->want_accept && !s->accepting)
                ospoll_accept(ospoll, s);
        }

        /* drain completions that are already available */
        ospoll_drain(ospoll, 0);

        /* deliver callbacks for ready file descriptors */
        nready = 0;
        for (ospoll->iterator = 0; ospoll->iterator < ospoll->num;
             ospoll->iterator++) {
            struct ospollfd *osfd = &ospoll->osfds[ospoll->iterator];
            struct sockbuf *s = osfd->sockbuf;
            int xevents = 0;

            if (!s)
                continue;
            if (osfd->want_read &&
                (s->bufrd->avail > s->bufrd->written || s->eof))
                xevents |= X_NOTIFY_READ;
            if (osfd->want_write && s->bufwr->avail < s->bufwr->alloced)
                xevents |= X_NOTIFY_WRITE;
            if (osfd->want_accept && s->accept_head)
                xevents |= X_NOTIFY_READ;
            if (xevents) {
                osfd->callback(osfd->fd, xevents, osfd->data);
                nready++;
            }
        }
        if (nready > 0)
            return nready;

        /* nothing ready yet: wait for a completion or a Windows message */
        {
            DWORD ms, elapsed;

            if (timeout < 0)
                ms = INFINITE;
            else {
                elapsed = GetTickCount() - start;
                if (elapsed >= (DWORD) timeout)
                    return 0;
                ms = (DWORD) timeout - elapsed;
            }
            if (MsgWaitForMultipleObjects(1, &ospoll->iocp_handle, FALSE, ms,
                                          QS_ALLINPUT) == WAIT_OBJECT_0)
                continue;       /* completions arrived; loop to drain + deliver */
            return 0;           /* message, timeout, or error */
        }
    }
#endif
#if POLL
    nready = xserver_poll(ospoll->fds, ospoll->num, timeout);
    ospoll->changed = FALSE;
    if (nready > 0) {
        int f;
        for (f = 0; f < ospoll->num; f++) {
            short revents = ospoll->fds[f].revents;
            short oldevents = ospoll->osfds[f].revents;

            ospoll->osfds[f].revents = (revents & (POLLIN|POLLOUT));
            if (ospoll->osfds[f].trigger == ospoll_trigger_edge)
                revents &= ~oldevents;
            if (revents) {
                int    xevents = 0;
                if (revents & POLLIN)
                    xevents |= X_NOTIFY_READ;
                if (revents & POLLOUT)
                    xevents |= X_NOTIFY_WRITE;
                if (revents & (~(POLLIN|POLLOUT)))
                    xevents |= X_NOTIFY_ERROR;
                ospoll->osfds[f].callback(ospoll->fds[f].fd, xevents,
                                          ospoll->osfds[f].data);

                /* Check to see if the arrays have changed, and just go back
                 * around again
                 */
                if (ospoll->changed)
                    break;
            }
        }
    }
#endif
    return nready;
}

void
ospoll_reset_events(struct ospoll *ospoll, int fd)
{
#if POLLSET
    int pos = ospoll_find(ospoll, fd);

    if (pos < 0)
        return;

    ospoll->fds[pos].revents = 0;
#endif
#if PORT
    int pos = ospoll_find(ospoll, fd);

    if (pos < 0)
        return;

    epoll_mod(ospoll, ospoll->fds[pos]);
#endif
#if POLL
    int pos = ospoll_find(ospoll, fd);

    if (pos < 0)
        return;

    ospoll->osfds[pos].revents = 0;
#endif
#if WIN32POLL
    /* readiness is level-triggered, so there is nothing to re-arm */
    (void) ospoll;
    (void) fd;
#endif
}

void *
ospoll_data(struct ospoll *ospoll, int fd)
{
    int pos = ospoll_find(ospoll, fd);

    if (pos < 0)
        return NULL;
#if POLLSET
    return ospoll->fds[pos].data;
#endif
#if EPOLL || PORT
    return ospoll->fds[pos]->data;
#endif
#if POLL || WIN32POLL
    return ospoll->osfds[pos].data;
#endif
}

void CheckServerConnections(struct ospoll *server_poll)
{
#if WIN32POLL
    int i;

    for (i = server_poll->num - 1; i >= 0; i--) {
        struct ospollfd *osfd = &server_poll->osfds[i];
        struct sockbuf *s = osfd->sockbuf;

        if (osfd->callback == ClientReady && s && s->eof)
            CloseDownClient((ClientPtr) osfd->data);
    }
#endif
}
