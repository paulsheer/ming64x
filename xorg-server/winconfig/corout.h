/* corout.h - Paul Sheer <paulsheer@gmail.com>  */

#ifdef _WIN32

#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <error.h>
#define ioctl(a,b,c)        ioctlsocket(a,b,c)
WINSOCK_API_LINKAGE INT WSAAPI inet_pton(INT Family,
                                         LPCSTR pStringBuf, PVOID pAddr);
WINSOCK_API_LINKAGE LPCSTR WSAAPI inet_ntop(INT Family,
                         LPCVOID pAddr, LPSTR pStringBuf, size_t StringBufSize);

typedef int socklen_t;
typedef unsigned long ioctlopt_t;
typedef BOOL sockoptparam_t;
typedef SOCKET socket_t;
#define OS_SOCK_ERR     ((socket_t) -1)
#define RETRY \
 (WSAGetLastError() == WSAEALREADY || WSAGetLastError() == WSAEINPROGRESS || \
  WSAGetLastError() == WSAEINTR || WSAGetLastError() == WSAEWOULDBLOCK)
#undef perror
#define perror(e)     \
      fprintf(stderr, "%s: [%s]\n", (e), mswin_error_to_text(WSAGetLastError()))

#else

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
typedef int ioctlopt_t;
typedef int sockoptparam_t;
typedef int socket_t;
#define OS_SOCK_ERR     ((socket_t) -1)
#define closesocket(s)  close(s)
#define RETRY \
 (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK || \
  errno == EINPROGRESS || errno == ENOTCONN)

#endif

#include <errno.h>
#ifdef HAVE_SETJMP
#include <setjmp.h>
#endif

struct coroute;

enum corout_result {
    COROUT_NULL = 0,
    COROUT_YIELD = 1,
    COROUT_ERROR = 2,
};

#define COROUT_MAX_STACK_DEPTH        64

#define CO_EV_START     1
#define CO_EV_RD        2
#define CO_EV_WR        3
#define CO_EV_CONNECT   4
#define CO_EV_ACCEPT    5
#define CO_EV_TIMER     6
#define CO_EV_SIG       7

struct corout_item;
struct corout;
struct sockbuf;
struct sockevent;
struct socket;

typedef void (*corout_fn_t) (struct corout_item *p, void *user_data,
                             const struct sockevent *ev);
typedef void (*corout_free_fn_t) (void *);

struct polling_info;

union sockaddr_in4in6 {
    struct sockaddr sa;
    struct sockaddr_in sain4;
    struct sockaddr_in6 sain6;
    struct sockaddr_storage ss;
};

struct accept_sock {
    struct socket *new_client;
    union sockaddr_in4in6 addr;
    socklen_t addrlen;
};

struct sockevent {
    struct socket *e;
    int ev;
    struct accept_sock *accept_sock;
};

#define COROUT_BUFFER_ALLOCED           (128 * 1024)

struct buffer {
    int ref;
    char *data;
    char reading;
    char writing;
    int written;
    int avail;
    int alloced;
    int io_ops;
};

#ifndef COROUT_C
struct sockbuf {
    struct buffer *bufrd, *bufwr;
    /* rest is private */
};
#endif

struct socket {
    struct sockbuf *s;
    int unlinked;
};

enum corout_socket_type {
    COROUT_SOCKET_TYPE_TCP = 1,
};

#if __GNUC__ >= 9
#define WARN_UNUSED     __attribute__((warn_unused_result))
#else
#define WARN_UNUSED
#endif

int inaddr_cmp(union sockaddr_in4in6 *a, union sockaddr_in4in6 *b);
char *inaddr_str(union sockaddr_in4in6 *a, char *str, int *port);
int inaddr_validate(const char *s, int *family);
void inaddr_from_text(union sockaddr_in4in6 *r, const char *s, long port,
                      int family);
void corout_init(void);
struct corout *corout_alloc(void);
void corout_free(struct corout *o);
/* corout1 {{{ */
int corout_add(struct corout *o, corout_fn_t fn,
                 corout_free_fn_t free_fn, void *user_data);
/* }}} */
int corout_count(struct corout *o);
void corout_socket_link(struct corout_item *c, struct socket *e);
struct socket *corout_socket_listener_alloc(struct corout *o,
           enum corout_socket_type type, long bind_port, const char *bind_addr);
struct socket *corout_socket_client_alloc(struct corout *o,
                                   enum corout_socket_type type, long bind_port,
                                const char *bind_addr, const char *remote_addr);
int corout_socket_local_addr(struct socket *e, union sockaddr_in4in6 *addr);
int corout_no_delay(struct socket *s);
void corout_socket_free(struct socket *);
int corout_socket_count(struct corout_item *c);
int corout_accept(struct socket *s) WARN_UNUSED;
int corout_connect(struct socket *s, long port, const char *saddr);
int corout_connect_success(struct socket *e);
void corout_error_str(int error, char *s, int n);
void corout_read(struct socket *s);
void corout_write(struct socket *s);
void corout_readwrite(struct socket *s);
void corout_clear(struct socket *s);
int corout_is_writing(struct socket *s);
void corout_wait_timeout(struct corout_item *p, unsigned long ms,
                         int clear_io_waits);
void corout_wait_wakeable(struct corout_item *p, unsigned long ms);
void corout_run(struct corout *o);
void corout_signal(struct corout *o, void *user_data,
                   unsigned long user_signal);
void corout_get_signal(unsigned long *sig, struct corout_item *o);
void corout_kill(struct corout *o, void *user_data);
#ifdef _WIN32
HANDLE corout_iocp_handle(struct corout *o);
#endif
struct buffer *corout_buffer_alloc(int n);
void corout_buffer_free(struct buffer *p);


struct tiny_frame {
    int line_no;
    int fullpacket_packet_type; /* <=== this is the only local variable which is as risk of overwriting due to re-entrance */
    unsigned char msg;
};

#ifndef COROUT_C

struct corout_item {
    struct corout *o;
    struct tiny_frame stack[COROUT_MAX_STACK_DEPTH];
    int depth;
    enum corout_result result;
#ifdef HAVE_SETJMP
    jmp_buf rollup;
#endif
    /* rest is private */
};
#endif

#define START() \
    switch (state->stack[state->depth].line_no) { \
    case 0:

#define END() \
        state->stack[state->depth].line_no = 0; \
        if (!state->depth) { \
            state->result = COROUT_ERROR; \
            return; \
        } \
    case __LINE__: \
        goto __error_return; /* prevent 'unused label' warning */ \
        __error_return:; \
        /* fall out here, allowing users to inspect errors */ \
    }

#ifdef HAVE_SETJMP
#define YIELD_() \
        state->result = COROUT_YIELD; \
        state->stack[state->depth].line_no = __LINE__; \
        state->depth = 0; \
        longjmp(state->rollup, 1); \
    case __LINE__:
#else
#define YIELD_() \
        state->result = COROUT_YIELD; \
        state->stack[state->depth].line_no = __LINE__; \
        return; \
    case __LINE__:
#endif

#define CALL(f) \
    do { \
        state->stack[state->depth].line_no = __LINE__; \
        state->stack[state->depth + 1].line_no = 0; \
        /* fall through */ \
    case __LINE__: \
        state->depth++; \
        state->result = COROUT_NULL; \
        f; \
        state->depth--; \
        if (state->result == COROUT_YIELD) \
            return; \
        if (state->result == COROUT_ERROR) \
            goto __error_return; \
    } while(0)

#define WAIT_ACCEPT(e) \
    do { \
        if (!(e)->s) \
            COROUT_EXIT(); \
        if (corout_accept(e)) \
            COROUT_EXIT(); \
        YIELD_(); \
    } while(0)

#define WAIT_READ(e) \
    do { \
        if (!(e)->s) \
            COROUT_EXIT(); \
        corout_read(e); \
        YIELD_(); \
        if (!(e)->s) \
            COROUT_EXIT(); \
    } while(0)

#define GETCHAR(e,ch) \
    do { \
        if (!(e)->s) \
            COROUT_EXIT(); \
        if ((e)->s->bufrd->avail > (e)->s->bufrd->written) \
            *(ch) = (e)->s->bufrd->data[(e)->s->bufrd->written++]; \
        else { \
            WAIT_READ(e); \
            assert ((e)->s->bufrd->avail > (e)->s->bufrd->written); \
            *(ch) = (e)->s->bufrd->data[(e)->s->bufrd->written++]; \
        } \
    } while(0)

#define READ_BLOCK(e,block,blocklen) \
    do { \
        int __done; \
        do { \
            if (!(e)->s) \
                COROUT_EXIT(); \
            if ((e)->s->bufrd->avail >= (e)->s->bufrd->written + (blocklen)) { \
                memcpy((block), (e)->s->bufrd->data + (e)->s->bufrd->written, \
                                                                  (blocklen)); \
                (e)->s->bufrd->written += (blocklen); \
                __done = 1; \
            } else { \
                WAIT_READ(e); \
                __done = 0; \
            } \
        } while(!__done); \
    } while(0)

/* Read whatever bytes are already buffered (at least 1, at most blocklen) into
   block. Yields only when the read buffer is empty. On return *out_count holds
   the number of bytes copied (>= 1). This is the coroutine analogue of recv()
   used by the SSH transport layer, which must drain the stream in whatever
   chunk sizes the peer delivers rather than in exact READ_BLOCK amounts. */
#define READ_SOME(e, block, blocklen, out_count) \
    do { \
        int __n; \
        if (!(e)->s) \
            COROUT_EXIT(); \
        if ((e)->s->bufrd->avail == (e)->s->bufrd->written) \
            WAIT_READ(e); \
        __n = (e)->s->bufrd->avail - (e)->s->bufrd->written; \
        if (__n > (int)(blocklen)) \
            __n = (int)(blocklen); \
        memcpy((block), (e)->s->bufrd->data + (e)->s->bufrd->written, \
               (size_t)__n); \
        (e)->s->bufrd->written += __n; \
        *(out_count) = __n; \
    } while(0)

/* Windows retry schedule is 1+2+4+8 */
#define CONNECT_DEFAULT_TIME        ((1+2+4+8)*1000 + 2500)
#define WAIT_CONNECT(ms, timeout, error, e, port, addr) \
    do { \
        if (!(e)->s) \
            COROUT_EXIT(); \
        if (corout_connect(e, port, addr)) { \
            *(timeout) = 0, *(error) = corout_connect_success(e); \
        } else { \
            corout_wait_timeout(state, (ms), 0); \
            YIELD_(); \
            if (ev->ev == CO_EV_TIMER) \
                *(timeout) = 1, *(error) = 0; \
            else \
                *(timeout) = 0, *(error) = corout_connect_success(e); \
        } \
    } while(0)

#define WAIT_WRITE(e) \
    do { \
        if (!(e)->s) \
            COROUT_EXIT(); \
        while ((e)->s->bufwr->written < (e)->s->bufwr->avail) { \
            corout_write(e); \
            YIELD_(); \
            if (!(e)->s) \
                COROUT_EXIT(); \
        } \
    } while(0)

/* Append a byte chunk to the socket's write buffer (analogous to outln()).
   Follow with WAIT_WRITE() to flush it. The caller must keep each chunk within
   the free space of bufwr; a large write is a loop of APPEND_BLOCK+WAIT_WRITE. */
#define APPEND_BLOCK(e, block, blocklen) \
    do { \
        struct buffer *__b; \
        if (!(e)->s) \
            COROUT_EXIT(); \
        __b = (e)->s->bufwr; \
        assert(__b->alloced - __b->avail >= (int)(blocklen)); \
        memcpy(__b->data + __b->avail, (block), (blocklen)); \
        __b->avail += (blocklen); \
    } while(0)

#define MILLISLEEP(usig, u) \
    do { \
        corout_wait_timeout(state, u, 1); \
        YIELD_(); \
        corout_get_signal(usig, state); \
    } while(0)

#define COROUT_EXIT() \
    do { \
        state->result = COROUT_ERROR; \
        return; \
    } while (0)

#define YIELD() \
    do { \
        corout_wait_timeout(state, 0L, 1); \
        YIELD_(); \
    } while (0)

