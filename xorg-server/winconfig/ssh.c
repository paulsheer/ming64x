#include "ssh.h"

#include <assert.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "libssh2_priv.h"
#include "transport.h"
#include "channel.h"

#include "terminal.h"

/* coroutine I/O seam: START/END/CALL/WAIT_CONNECT/WAIT_WRITE/READ_SOME etc. */
#include "corout.h"
/* corout.h leaks socket helper macros that would rewrite libssh2's own
   ioctl/perror calls; keep only the coroutine macros. */
#undef ioctl
#undef perror
#undef RETRY

/* Like the CALL macro, but a hard error (COROUT_EXIT) anywhere in the callee jumps to
   ssh_run's own cleanup label instead of unwinding out of the coroutine. This
   keeps the session-free cleanup reachable on every error path while staying
   inside the START/END switch (CALL cannot be used after END()). */
#define CALL_SOFT(f) \
    do { \
        state->stack[state->depth].line_no = __LINE__; \
        state->stack[state->depth + 1].line_no = 0; \
    case __LINE__: \
        state->depth++; \
        state->result = COROUT_NULL; \
        f; \
        state->depth--; \
        if (state->result == COROUT_YIELD) \
            return; \
        if (state->result == COROUT_ERROR) \
            goto out; \
    } while(0)

#define SSH_IN_CAP     (64 * 1024)
#define SSH_OUT_CAP    (16 * 1024)
#define SSH_POLL_MS    50     /* ms; idle poll interval for the worker */
#define SSH_SEND_CHUNK 8192

/* X11 forwarding relay (SSH x11 channel <-> local VcXsrv socket) */
#define X11_MAX         1024
#define X11_POLL_MS     2000
#define X11_SIG         1
#define X11_FORWARD_DISPLAY 10   /* remote DISPLAY number we request (screen 0) */

/* PulseAudio reverse-forward relay (SSH forwarded-tcpip <-> local daemon) */
#define AUDIO_MAX       32
#define AUDIO_COOKIE_LEN 256     /* PA_NATIVE_COOKIE_LENGTH (bytes) */

/* ---- byte queue ---- */

static void
byteq_init(byteq *q, const size_t cap, const int block)
{
    InitializeCriticalSection(&q->lock);
    InitializeConditionVariable(&q->not_full);
    q->buf = (unsigned char*)malloc(cap);
    q->cap = cap;
    q->head = q->tail = q->n = 0;
    q->block = block;
    q->closed = 0;
}

static void
byteq_free(byteq *q)
{
    free(q->buf);
    q->buf = NULL;
    q->cap = q->head = q->tail = q->n = 0;
    DeleteCriticalSection(&q->lock);
}

/* blocking producer when block != 0, otherwise drop-oldest on overflow */
static void
byteq_push(byteq *q, const unsigned char *p, size_t n)
{
    size_t i;
    if (n > q->cap)
        n = q->cap;
    EnterCriticalSection(&q->lock);

    if (q->block) {
        while (q->n + n > q->cap && !q->closed)
            SleepConditionVariableCS(&q->not_full, &q->lock, INFINITE);
        if (q->closed) {
            LeaveCriticalSection(&q->lock);
            return;
        }
    } else {
        while (n > q->cap - q->n) {
            q->head = (q->head + 1) % q->cap;
            q->n--;
        }
    }

    for (i = 0; i < n; ++i) {
        q->buf[q->tail] = p[i];
        q->tail = (q->tail + 1) % q->cap;
    }
    q->n += n;
    LeaveCriticalSection(&q->lock);
}

static size_t
byteq_pop(byteq *q, unsigned char *dst, const size_t max)
{
    size_t got = 0;
    EnterCriticalSection(&q->lock);
    while (got < max && q->n > 0) {
        dst[got++] = q->buf[q->head];
        q->head = (q->head + 1) % q->cap;
        q->n--;
    }
    if (got > 0)
        WakeAllConditionVariable(&q->not_full);
    LeaveCriticalSection(&q->lock);
    return got;
}

static size_t
byteq_space(byteq *q)
{
    size_t s;
    EnterCriticalSection(&q->lock);
    s = q->cap - q->n;
    LeaveCriticalSection(&q->lock);
    return s;
}

/* ---- worker thread ---- */

static void
ssh_report(ssh_session *s, const char *text)
{
    byteq_push(&s->in, (const unsigned char*)text, strlen(text));
}

/* Record a DISPLAY setup error for the UI thread to show. */
static void
set_display_error(ssh_session *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->display_error, sizeof s->display_error, fmt, ap);
    va_end(ap);
    InterlockedExchange(&s->display_error_pending, 1);
}

/* ---- host-key verification (known_hosts + prompt) ---- */

static const char b64_alpha[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
b64_encode(const unsigned char *in, const size_t len, char *out, const size_t cap)
{
    size_t i, o = 0;

    if (cap < 4)
        return 0;
    for (i = 0; i + 2 < len; i += 3) {
        if (o + 4 >= cap)
            return 0;
        out[o++] = b64_alpha[in[i] >> 2];
        out[o++] = b64_alpha[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
        out[o++] = b64_alpha[((in[i + 1] & 0x0f) << 2) | (in[i + 2] >> 6)];
        out[o++] = b64_alpha[in[i + 2] & 0x3f];
    }
    if (i < len) {
        if (o + 4 >= cap)
            return 0;
        out[o++] = b64_alpha[in[i] >> 2];
        if (i + 1 < len) {
            out[o++] = b64_alpha[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
            out[o++] = b64_alpha[(in[i + 1] & 0x0f) << 2];
            out[o++] = '=';
        } else {
            out[o++] = b64_alpha[(in[i] & 0x03) << 4];
            out[o++] = '=';
            out[o++] = '=';
        }
    }
    out[o] = '\0';
    return 1;
}

static const char *
hostkey_type_name(const int type)
{
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:        return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:  return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:  return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:  return "ecdsa-sha2-nistp521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:    return "ssh-ed25519";
    default:                              return "unknown";
    }
}

static int
known_hosts_path(char *buf, const size_t cap)
{
    const char *prof = getenv("USERPROFILE");
    if (!prof || !*prof)
        return 0;
    return snprintf(buf, cap, "%s\\.ssh\\known_hosts", prof) < (int)cap;
}

/* Does a comma-separated host pattern from a known_hosts line match `host`? */
static int
hostspec_matches(const char *spec, const char *host)
{
    char bracketed[1024];
    const char *p = spec;

    snprintf(bracketed, sizeof bracketed, "[%s]:22", host);

    while (*p) {
        const char *start = p;
        size_t len;

        while (*p && *p != ',')
            p++;
        len = (size_t)(p - start);

        if (len > 0 && start[0] != '|') {   /* skip hashed entries */
            if (host[len] == '\0' && strncmp(start, host, len) == 0)
                return 1;
            if (bracketed[len] == '\0' && strncmp(start, bracketed, len) == 0)
                return 1;
        }
        if (*p == ',')
            p++;
    }
    return 0;
}

/* returns 1 = known & matching, 2 = known but changed, 0 = not found */
static int
known_hosts_check(const char *host, const char *ktype, const char *b64)
{
    char path[MAX_PATH];
    FILE *f;
    char line[2048];
    int found_type = 0;

    if (!known_hosts_path(path, sizeof path))
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;

    while (fgets(line, sizeof line, f)) {
        char *hostspec, *kt, *key, *p;

        line[strcspn(line, "\r\n")] = '\0';

        hostspec = line;
        while (*hostspec == ' ' || *hostspec == '\t') hostspec++;
        if (!*hostspec) continue;
        p = hostspec;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p) continue;
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        kt = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p) continue;
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        key = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        *p = '\0';

        if (strcmp(kt, ktype) != 0)
            continue;
        if (!hostspec_matches(hostspec, host))
            continue;

        found_type = 1;
        if (strcmp(key, b64) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return found_type ? 2 : 0;
}

static int
known_hosts_add(const char *host, const char *ktype, const char *b64)
{
    char dir[MAX_PATH];
    char path[MAX_PATH];
    const char *prof = getenv("USERPROFILE");
    FILE *f;

    if (!prof || !*prof)
        return 0;
    snprintf(dir, sizeof dir, "%s\\.ssh", prof);
    CreateDirectoryA(dir, NULL);        /* no-op if it already exists */

    if (!known_hosts_path(path, sizeof path))
        return 0;
    f = fopen(path, "ab");
    if (!f)
        return 0;
    fprintf(f, "%s %s %s\n", host, ktype, b64);
    fclose(f);
    return 1;
}

/* Verify the server key against known_hosts. Blocks on a prompt (answered by
   the UI thread) if the key is unknown. Returns 1 to continue, 0 to abort. */
static int
hostkey_verify(ssh_session *s, LIBSSH2_SESSION *session)
{
    const char *hk;
    const char *hash;
    size_t hk_len;
    int hk_type;
    const char *ktype;
    char b64[4096];
    char fp[128];
    int status;

    hk = libssh2_session_hostkey(session, &hk_len, &hk_type);
    if (!hk) {
        ssh_report(s, "hostkey: could not retrieve server key\r\n");
        return 0;
    }
    ktype = hostkey_type_name(hk_type);
    if (!b64_encode((const unsigned char*)hk, hk_len, b64, sizeof b64)) {
        ssh_report(s, "hostkey: key too large to encode\r\n");
        return 0;
    }

    hash = (const char*)libssh2_hostkey_hash(session,
        LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash) {
        char tmp[64];
        b64_encode((const unsigned char*)hash, 32, tmp, sizeof tmp);
        snprintf(fp, sizeof fp, "SHA256:%s", tmp);
    } else {
        snprintf(fp, sizeof fp, "(unavailable)");
    }

    status = known_hosts_check(s->host, ktype, b64);
    if (status == 1)
        return 1;                       /* known and matching */

    if (status == 2) {
        char line[512];
        snprintf(line, sizeof line,
            "host key verification failed: the %s key for %s has changed\r\n"
            "(possible man-in-the-middle). Remove the old entry from\r\n"
            "%%USERPROFILE%%\\.ssh\\known_hosts if the server was reinstalled.\r\n",
            ktype, s->host);
        ssh_report(s, line);
        set_display_error(s, "Host key for \"%s\" has changed "
            "(possible man-in-the-middle)", s->host);
        return 0;
    }

    /* unknown: hand the question to the UI thread and wait */
    snprintf(s->prompt_host, sizeof s->prompt_host, "%s", s->host);
    snprintf(s->prompt_keytype, sizeof s->prompt_keytype, "%s", ktype);
    snprintf(s->prompt_fingerprint, sizeof s->prompt_fingerprint, "%s", fp);

    InterlockedExchange(&s->prompt_pending, 1);
    EnterCriticalSection(&s->prompt_lock);
    while (s->prompt_pending && s->running)
        SleepConditionVariableCS(&s->prompt_cv, &s->prompt_lock, INFINITE);
    LeaveCriticalSection(&s->prompt_lock);

    if (!s->running)
        return 0;
    if (s->prompt_answer) {
        if (!known_hosts_add(s->host, ktype, b64))
            ssh_report(s, "hostkey: could not write known_hosts\r\n");
        return 1;
    }
    ssh_report(s, "host key verification failed\r\n");
    set_display_error(s, "Host key verification failed");
    return 0;
}

static void
ssh_report_error(ssh_session *s, LIBSSH2_SESSION *session, const char *stage)
{
    char *errmsg = NULL;
    int errlen = 0;
    int err = libssh2_session_last_error(session, &errmsg, &errlen, 0);
    char line[512];

    snprintf(line, sizeof line, "%s: failed (%d: %.*s)\r\n",
        stage, err, errlen, errmsg ? errmsg : "");
    ssh_report(s, line);
    set_display_error(s, "%s failed (%d: %.*s)",
        stage, err, errlen, errmsg ? errmsg : "");
}

struct ssh_ctx;

/* One forwarded X11 connection. The VcXsrv socket is owned by x11_runner (a
   pure corout.h coroutine); the LIBSSH2 x11 channel is owned and serviced by
   ssh_run (the only coroutine permitted to touch libssh2). Data crosses the
   two buffers. All relay progress lives here so it survives both coroutines'
   yields. */
typedef struct x11_conn {
    LIBSSH2_CHANNEL *channel;
    struct socket *xsock;
    int display_number;             /* VcXsrv display :0..:12 */
    struct ssh_ctx *ctx;            /* ssh_run's user_data (signal target) */

    int rd_want;
    int wr_n;
    struct buffer *relay_buf;       /* held (ref++) across a channel write/read */
    int ssh_ops;
    int chan_done;                  /* ssh_run freed the channel */
    int rd_space;
    int wr_data;
    int sock_done;                  /* runner exited */
} x11_conn;

/* One PulseAudio reverse-forwarded connection: the SSH forwarded-tcpip channel
   (remote app <-> us) is relayed to a plain TCP socket (us <-> the local
   PulseAudio daemon).  The socket is owned by audio_runner (a pure corout.h
   coroutine); the channel is owned and serviced by ssh_run.  Data crosses the
   two buffers, exactly like the X11 relay. */
typedef struct audio_conn {
    LIBSSH2_CHANNEL *channel;       /* forwarded-tcpip: remote app <-> us */

    struct socket *xsock;           /* plain TCP to the local PulseAudio daemon */
    int pulseaudio_port;            /* local daemon TCP port (default 4713) */
    struct ssh_ctx *ctx;            /* ssh_run's user_data (signal target) */

    int rd_want;
    int wr_n;
    struct buffer *relay_buf;       /* held (ref++) across a channel write/read */
    int ssh_ops;
    int chan_done;                  /* ssh_run freed the channel */
    int rd_space;
    int wr_data;
    int sock_done;                  /* runner exited */
} audio_conn;

/* Per-connection state that must survive a coroutine yield. corout_step()
   re-enters ssh_run() fresh on every resume, so any value held in a C local of
   ssh_run() is reset after each YIELD_. Everything that carries progress
   across a yield (the libssh2 session/channel/socket pointers and the relay
   buffers/counters) lives here instead, in the worker's own stack frame which
   longjmp does not unwind. */
struct ssh_ctx {
    ssh_session *s;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    struct socket *sock;
    const char *stage;
    char fail_hint[192];        /* if non-empty, overrides the red UI error */
    LIBSSH2_SESSION *tofree;    /* session held across the free's own yields */

    /* outbound chunk being written (partial writes span yields) */
    unsigned char wr_buf[SSH_SEND_CHUNK];
    size_t wr_n;        /* bytes loaded into wr_buf */
    size_t wr_off;      /* bytes of wr_buf already written */

    /* inbound read target (buflen must stay stable across the read's yield) */
    char rd_buf[SSH_SEND_CHUNK];
    size_t rd_want;

    /* one-shot setup values: CALL_SOFT re-evaluates its argument on resume, so
       these must live in ctx rather than in a C local of ssh_run() */
    char disp[80];      /* DISPLAY string built before setenv */
    int pty_w, pty_h;   /* pty dimensions built before request_pty */

    /* active forwarded X11 connections (serviced round-robin) */
    struct x11_conn *x11[X11_MAX];
    int x11_rr;
    struct x11_conn *x11_cur;

    /* PulseAudio reverse-forward (listener + active relay connections) */
    LIBSSH2_LISTENER *audio_listener;
    struct audio_conn *audio[AUDIO_MAX];
    int audio_rr;
    struct audio_conn *audio_cur;
    size_t audio_off;                       /* bytes of cookie already written */
    LIBSSH2_CHANNEL *exec_channel;          /* one-shot cookie-write channel */
    char audio_path[512];                   /* concrete remote cookie path */
    int audio_path_len;
};

/* Relay one forwarded X11 connection: VcXsrv socket <-> SSH x11 channel. Owns
   the VcXsrv socket (pure corout.h I/O); the channel is serviced by ssh_run.
   Data crosses the two buffers. The runner is always timer-wakeable while idle
   so ssh_run's corout_signal can interrupt it (e.g. to notice chan_done). */
static void
x11_runner(struct corout_item *state, void *user_data, const struct sockevent *ev)
{
    x11_conn *x = (x11_conn *)user_data;
    struct ssh_ctx *ctx = x->ctx;

    START();

    x->xsock = corout_socket_client_alloc(state->o, COROUT_SOCKET_TYPE_TCP,
                                          0, NULL, "127.0.0.1");
    if (!x->xsock)
        COROUT_EXIT();
    corout_socket_link(state, x->xsock);
    corout_no_delay(x->xsock);
    {
        int timeout = 0, cerr = 0;
        WAIT_CONNECT(6000, &timeout, &cerr, x->xsock,
                     6000 + x->display_number, "127.0.0.1");
        if (timeout || cerr)
            COROUT_EXIT();
    }

    for (;;) {
        if (x->chan_done || !x->xsock->s)
            break;

        x->wr_data = x->xsock->s->bufwr->avail - x->xsock->s->bufwr->written;
        assert(x->wr_data >= 0);
        x->rd_space = x->xsock->s->bufrd->alloced - x->xsock->s->bufrd->avail;
        assert(x->rd_space >= 0);

        int can_read = x->rd_space || x->xsock->s->bufrd->written == x->xsock->s->bufrd->avail;

        if (can_read && x->wr_data) {
            corout_readwrite(x->xsock);
        } else if (x->wr_data) {
            corout_write(x->xsock);
        } else if (can_read) {
            corout_read(x->xsock);
        } else {
            corout_clear(x->xsock); /* no-op on Windows */
        }

        corout_wait_wakeable(state, X11_POLL_MS);
        YIELD_();

        if (x->chan_done || !x->xsock->s)
            break;

        int work_done =
            (x->rd_space != x->xsock->s->bufrd->alloced - x->xsock->s->bufrd->avail) ||
            (x->wr_data != x->xsock->s->bufwr->avail - x->xsock->s->bufwr->written);
        if (work_done)
            corout_signal(state->o, ctx, X11_SIG);
    }

    END();
}

static void
x11_runner_free(void *user_data)
{
    x11_conn *x = (x11_conn *)user_data;
    if (x->xsock)
        corout_socket_free(x->xsock);
    x->xsock = NULL;
    x->sock_done = 1;
}

/* Invoked (via SSH2_X11_OPEN) when the server opens an x11 channel. Runs on
   ssh_run's coroutine stack, so it must not call any libssh2 function (that
   would corrupt the shared stack[]). It only records the channel and spawns
   the relay coroutine; ssh_run services the channel later. */
static void
x11_open_cb(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
            const char *shost, const int sport, void **abstract)
{
    struct ssh_ctx *ctx = (struct ssh_ctx *)*abstract;
    struct x11_conn *x;
    int i;

    (void)shost;
    (void)sport;

    for (i = 0; i < X11_MAX; i++)
        if (!ctx->x11[i])
            break;
    if (i == X11_MAX)
        return;         /* no slot; channel stays linked until session free */

    x = (struct x11_conn *)malloc(sizeof *x);
    if (!x)
        return;
    memset(x, 0, sizeof *x);

    x->channel = channel;
    x->display_number = ctx->s->display_number;
    x->ctx = ctx;
    ctx->x11[i] = x;
    ctx->s->x11_open_count++;

    corout_add(session->corout_state->o, x11_runner, x11_runner_free, x);
}






/* Relay one forwarded PulseAudio connection: local daemon socket <-> SSH
   forwarded-tcpip channel. Owns the daemon socket (pure corout.h I/O); the
   channel is serviced by ssh_run. Data crosses the two buffers. The runner is
   always timer-wakeable while idle so ssh_run's corout_signal can interrupt it
   (e.g. to notice chan_done). */
static void
audio_runner(struct corout_item *state, void *user_data, const struct sockevent *ev)
{
    audio_conn *x = (audio_conn *)user_data;
    struct ssh_ctx *ctx = x->ctx;

    START();

    x->xsock = corout_socket_client_alloc(state->o, COROUT_SOCKET_TYPE_TCP,
                                          0, NULL, "127.0.0.1");
    if (!x->xsock)
        COROUT_EXIT();
    corout_socket_link(state, x->xsock);
    corout_no_delay(x->xsock);
    {
        int timeout = 0, cerr = 0;
        WAIT_CONNECT(6000, &timeout, &cerr, x->xsock,
                     x->pulseaudio_port, "127.0.0.1");
        if (timeout || cerr)
            COROUT_EXIT();
    }

    for (;;) {
        if (x->chan_done || !x->xsock->s)
            break;

        x->wr_data = x->xsock->s->bufwr->avail - x->xsock->s->bufwr->written;
        assert(x->wr_data >= 0);
        x->rd_space = x->xsock->s->bufrd->alloced - x->xsock->s->bufrd->avail;
        assert(x->rd_space >= 0);

        int can_read = x->rd_space || x->xsock->s->bufrd->written == x->xsock->s->bufrd->avail;

        if (can_read && x->wr_data) {
            corout_readwrite(x->xsock);
        } else if (x->wr_data) {
            corout_write(x->xsock);
        } else if (can_read) {
            corout_read(x->xsock);
        } else {
            corout_clear(x->xsock); /* no-op on Windows */
        }

        corout_wait_wakeable(state, X11_POLL_MS);
        YIELD_();

        if (x->chan_done || !x->xsock->s)
            break;

        int work_done =
            (x->rd_space != x->xsock->s->bufrd->alloced - x->xsock->s->bufrd->avail) ||
            (x->wr_data != x->xsock->s->bufwr->avail - x->xsock->s->bufwr->written);
        if (work_done)
            corout_signal(state->o, ctx, X11_SIG);
    }

    END();
}

static void
audio_runner_free(void *user_data)
{
    audio_conn *x = (audio_conn *)user_data;
    if (x->xsock)
        corout_socket_free(x->xsock);
    x->xsock = NULL;
    x->sock_done = 1;
}

/* Invoked by ssh_run after accepting a forwarded-tcpip connection. Runs on
   ssh_run's coroutine stack, so it must not call any libssh2 function (that
   would corrupt the shared stack[]). It only records the channel and spawns
   the relay coroutine; ssh_run services the channel later. */
static void
audio_open_cb(LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
            const char *shost, const int sport, void **abstract)
{
    struct ssh_ctx *ctx = (struct ssh_ctx *)*abstract;
    struct audio_conn *x;
    int i;

    (void)shost;
    (void)sport;

    for (i = 0; i < AUDIO_MAX; i++)
        if (!ctx->audio[i])
            break;
    if (i == AUDIO_MAX)
        return;         /* no slot; channel stays linked until session free */

    x = (struct audio_conn *)malloc(sizeof *x);
    if (!x)
        return;
    memset(x, 0, sizeof *x);

    x->channel = channel;
    x->pulseaudio_port = ctx->s->audio_port;
    x->ctx = ctx;
    ctx->audio[i] = x;

    corout_add(session->corout_state->o, audio_runner, audio_runner_free, x);
}



/* The SSH connection as a coroutine: connects, negotiates, authenticates,
   opens a shell channel, then relays bytes between the byte queues and the
   channel for as long as s->running stays set. On any terminal error it frees
   the session (which closes and frees all channels) and terminates. */
static void
ssh_run(struct corout_item *state, void *user_data, const struct sockevent *ev)
{
    struct ssh_ctx *ctx = (struct ssh_ctx *)user_data;
    ssh_session *s = ctx->s;

    START();

    ctx->sock = corout_socket_client_alloc(state->o, COROUT_SOCKET_TYPE_TCP, 0,
                                           NULL, s->host);
    if (!ctx->sock) {
        ssh_report(s, "connect: host lookup failed\r\n");
        set_display_error(s, "Could not resolve host \"%s\"", s->host);
        goto out;
    }
    corout_socket_link(state, ctx->sock);
    s->iocp_handle = corout_iocp_handle(state->o);
    {
        int timeout = 0, cerr = 0;
        WAIT_CONNECT(CONNECT_DEFAULT_TIME, &timeout, &cerr, ctx->sock, 22,
                     s->host);
        if (timeout) {
            ssh_report(s, "connect: connection timed out\r\n");
            set_display_error(s, "Could not connect to \"%s\"", s->host);
            goto out;
        }
        if (cerr) {
            ssh_report(s, "connect: connection failed\r\n");
            set_display_error(s, "Could not connect to \"%s\"", s->host);
            goto out;
        }
    }

    ctx->session = libssh2_session_init();
    if (!ctx->session) {
        ssh_report(s, "session: init failed\r\n");
        goto out;
    }
    ctx->session->corout_state = state;
    ctx->session->corout_sock = ctx->sock;
    ctx->session->abstract = ctx;   /* x11_open_cb reads it back via *abstract */
    if (s->x11_forwarding)
        libssh2_session_callback_set(ctx->session, LIBSSH2_CALLBACK_X11,
                                     (void *)x11_open_cb);

    ctx->stage = "handshake";
    libssh2_session_set_last_error(ctx->session, 0, NULL);
    CALL_SOFT(libssh2_session_handshake(ctx->session));
    if (libssh2_session_last_errno(ctx->session))
        goto out;
    if (!hostkey_verify(s, ctx->session))
        goto out;

    ctx->stage = "authentication";
    libssh2_session_set_last_error(ctx->session, 0, NULL);
    CALL_SOFT(libssh2_userauth_password(ctx->session, s->username,
        s->password));
    if (libssh2_session_last_errno(ctx->session))
        goto out;

    ctx->stage = "channel open";
    libssh2_session_set_last_error(ctx->session, 0, NULL);
    CALL_SOFT(libssh2_channel_open_session(ctx->session));
    if (libssh2_session_last_errno(ctx->session))
        goto out;
    ctx->channel = ctx->session->open_channel;
    if (!ctx->channel) {
        ssh_report(s, "channel: open failed\r\n");
        goto out;
    }

    if (s->x11_forwarding) {
        /* Ask the server to forward X11 connections back over this session.
           auth_cookie is NULL so libssh2 generates a random cookie; this only
           reaches VcXsrv when it runs with access control disabled (-ac). A
           denied x11-req is fatal here (COROUT_EXIT unwinds to `out`). */
        ctx->stage = "x11 forwarding";
        libssh2_session_set_last_error(ctx->session, 0, NULL);
        CALL_SOFT(libssh2_channel_x11_req_ex(ctx->channel, 0, NULL, NULL, 0));
        ctx->stage = "channel open";    /* restore for the pty/shell steps */

        snprintf(ctx->disp, sizeof ctx->disp, "127.0.0.1:%d.0",
            X11_FORWARD_DISPLAY);
        libssh2_session_set_last_error(ctx->session, 0, NULL);
        ctx->stage = "setenv DISPLAY";
        snprintf(ctx->fail_hint, sizeof ctx->fail_hint,
            "Failed to set DISPLAY - add \"AcceptEnv DISPLAY\" to the "
            "server's sshd_config and restart sshd");
        CALL_SOFT(libssh2_channel_setenv_ex(ctx->channel, "DISPLAY", 7,
            ctx->disp, (unsigned int)strlen(ctx->disp)));
        ctx->fail_hint[0] = '\0';
    } else {
        union sockaddr_in4in6 la;
        char lip[64];

        if (corout_socket_local_addr(ctx->sock, &la) == 0) {
            inaddr_str(&la, lip, NULL);
            snprintf(ctx->disp, sizeof ctx->disp, "%s:%d.0", lip,
                s->display_number);
            libssh2_session_set_last_error(ctx->session, 0, NULL);
            ctx->stage = "setenv DISPLAY";
            snprintf(ctx->fail_hint, sizeof ctx->fail_hint,
                "Failed to set DISPLAY - add \"AcceptEnv DISPLAY\" to the "
                "server's sshd_config and restart sshd");
            CALL_SOFT(libssh2_channel_setenv_ex(ctx->channel, "DISPLAY", 7,
                ctx->disp, (unsigned int)strlen(ctx->disp)));
            ctx->fail_hint[0] = '\0';
        } else {
            set_display_error(s, "Could not determine local IP address");
        }
    }

    if (s->audio_enabled) {
        /* Ask the server to listen on its own loopback for PulseAudio and
           forward those connections back here; the relay connects them to the
           local daemon at 127.0.0.1:audio_port. */
        ctx->stage = "audio forward";
        libssh2_session_set_last_error(ctx->session, 0, NULL);
        CALL_SOFT(libssh2_channel_forward_listen_ex(ctx->session,
                    "127.0.0.1", s->audio_port, NULL, 16));
        ctx->audio_listener = ctx->session->fwdLstn_listener;
        if (!ctx->audio_listener) {
            set_display_error(s, "Could not set up PulseAudio forwarding");
        } else {
            char ps[64];

            snprintf(ps, sizeof ps, "tcp:127.0.0.1:%d", s->audio_port);
            libssh2_session_set_last_error(ctx->session, 0, NULL);
            ctx->stage = "setenv PULSE_SERVER";
            snprintf(ctx->fail_hint, sizeof ctx->fail_hint,
                "Failed to set PULSE_SERVER - add \"AcceptEnv "
                "PULSE_SERVER PULSE_COOKIE\" to the server's sshd_config "
                "and restart sshd");
            CALL_SOFT(libssh2_channel_setenv_ex(ctx->channel, "PULSE_SERVER", 12,
                ps, (unsigned int)strlen(ps)));
            ctx->fail_hint[0] = '\0';

            /* One-shot exec channel: install the cookie on the remote and
               report its concrete path on stdout (PULSE_COOKIE needs an
               absolute path; $HOME is expanded by the remote shell). */
            libssh2_session_set_last_error(ctx->session, 0, NULL);
            CALL_SOFT(libssh2_channel_open_session(ctx->session));
            ctx->exec_channel = ctx->session->open_channel;
            if (ctx->exec_channel) {
                static const char cookie_cmd[] =
                    "f=$HOME/.ming64x-pulse-cookie; umask 077; "
                    "cat > \"$f\" && chmod 600 \"$f\" && printf '%s' \"$f\"";

                CALL_SOFT(libssh2_channel_exec(ctx->exec_channel, cookie_cmd));

                for (ctx->audio_off = 0; ctx->audio_off < AUDIO_COOKIE_LEN; ) {
                    CALL_SOFT(libssh2_channel_write(ctx->exec_channel,
                        (const char *)s->audio_cookie + ctx->audio_off,
                        AUDIO_COOKIE_LEN - ctx->audio_off));
                    if (ctx->exec_channel->write_bytes <= 0)
                        break;
                    ctx->audio_off += (size_t)ctx->exec_channel->write_bytes;
                }
                CALL_SOFT(libssh2_channel_send_eof(ctx->exec_channel));

                ctx->audio_path_len = 0;
                for (;;) {
                    CALL_SOFT(libssh2_channel_read(ctx->exec_channel,
                        ctx->audio_path + ctx->audio_path_len,
                        sizeof ctx->audio_path - 1 - ctx->audio_path_len));
                    if (ctx->exec_channel->read_bytes <= 0)
                        break;
                    ctx->audio_path_len += (int)ctx->exec_channel->read_bytes;
                    if (ctx->audio_path_len >= (int)sizeof ctx->audio_path - 1)
                        break;
                }
                ctx->audio_path[ctx->audio_path_len] = '\0';

                CALL_SOFT(libssh2_channel_free(ctx->exec_channel));
                ctx->exec_channel = NULL;

                if (ctx->audio_path_len > 0) {
                    libssh2_session_set_last_error(ctx->session, 0, NULL);
                    ctx->stage = "setenv PULSE_COOKIE";
                    snprintf(ctx->fail_hint, sizeof ctx->fail_hint,
                        "Failed to set PULSE_COOKIE - add \"AcceptEnv "
                        "PULSE_SERVER PULSE_COOKIE\" to the server's sshd_config "
                        "and restart sshd");
                    CALL_SOFT(libssh2_channel_setenv_ex(ctx->channel,
                        "PULSE_COOKIE", 12, ctx->audio_path,
                        (unsigned int)ctx->audio_path_len));
                    ctx->fail_hint[0] = '\0';
                }
            }
        }
    }

    {
        /* Request a raw pty (OpenSSH-style): disable canonical mode, echo,
           and signal/flow control so the full ESC [ B sequence reaches the
           remote readline intact instead of being consumed or echoed back. */
        static const unsigned char raw_modes[] = {
#if 0
            53, 0, 0, 0, 0,  /* ECHO   = 0 */
            51, 0, 0, 0, 0,  /* ICANON = 0 */
            50, 0, 0, 0, 0,  /* ISIG   = 0 */
            38, 0, 0, 0, 0,  /* IXON   = 0 */
            59, 0, 0, 0, 0,  /* IEXTEN = 0 */
            36, 0, 0, 0, 0,  /* ICRNL  = 0 */
            42, 0, 0, 0, 1,  /* IUTF8  = 1 */
#endif
            0                   /* TTY_OP_END */
        };
        ctx->pty_w = s->resize_cols > 0 ? s->resize_cols : 80;
        ctx->pty_h = s->resize_rows > 0 ? s->resize_rows : 24;

        libssh2_session_set_last_error(ctx->session, 0, NULL);
        CALL_SOFT(libssh2_channel_request_pty_ex(ctx->channel, "xterm", 5,
            (const char *)raw_modes, sizeof raw_modes, ctx->pty_w, ctx->pty_h,
            0, 0));
        if (libssh2_session_last_errno(ctx->session)) {
            ssh_report(s, "channel: pty request failed\r\n");
            set_display_error(s, "Could not start remote shell");
            goto out;
        }
        libssh2_session_set_last_error(ctx->session, 0, NULL);
        CALL_SOFT(libssh2_channel_shell(ctx->channel));
        if (libssh2_session_last_errno(ctx->session)) {
            ssh_report(s, "channel: shell failed\r\n");
            set_display_error(s, "Could not start remote shell");
            goto out;
        }
    }

    ssh_report(s, "connected\r\n");
    ctx->stage = "connection";
    ctx->wr_n = 0;
    ctx->wr_off = 0;

    for (;;) {
        if (!s->running)
            break;

        /* ---- X11 forwarding: service one forwarded connection (round-robin) ---- */
        if (s->x11_forwarding) {
            for (ctx->x11_rr = 0; ctx->x11_rr < X11_MAX; ctx->x11_rr++) {
                if (!(ctx->x11_cur = ctx->x11[ctx->x11_rr]))
                    continue;
    
                /* 1. both sides finished: drop the connection */
                if (ctx->x11_cur->chan_done && ctx->x11_cur->sock_done) {
                    free(ctx->x11_cur);
                    ctx->x11[ctx->x11_rr] = NULL;
                    s->x11_open_count--;
                    continue;
                }
    
                /* 2. runner exited or VcXsrv socket gone: close the channel */
                if (ctx->x11_cur->sock_done || !ctx->x11_cur->xsock->s) {
                    if (ctx->x11_cur->channel) {
                        CALL_SOFT(libssh2_channel_free(ctx->x11_cur->channel));
                        ctx->x11_cur->channel = NULL;
                    }
                    ctx->x11_cur->chan_done = 1;
                    continue;
                }
    
                /* 3. remote closed: close channel, wake runner to exit */
                if (!ctx->x11_cur->channel ||
                            ctx->x11_cur->channel->remote.eof ||
                            ctx->x11_cur->channel->remote.close) {
                    if (ctx->x11_cur->channel) {
                        CALL_SOFT(libssh2_channel_free(ctx->x11_cur->channel));
                        ctx->x11_cur->channel = NULL;
                    }
                    ctx->x11_cur->chan_done = 1;
                    if (!ctx->x11_cur->sock_done)
                        corout_signal(state->o, ctx->x11_cur, X11_SIG);
                    continue;
                }
    
                /* 4a. relay ming64x.exe -> channel */
                ctx->x11_cur->ssh_ops = 0;
                ctx->x11_cur->relay_buf = ctx->x11_cur->xsock->s->bufrd;
                ctx->x11_cur->wr_n = ctx->x11_cur->relay_buf->avail -
                                     ctx->x11_cur->relay_buf->written;
                if (ctx->x11_cur->wr_n > 0) {
                    ctx->x11_cur->relay_buf->ref++;
                    ctx->x11_cur->relay_buf->writing = 1;
                    __sync_synchronize();
                    CALL_SOFT(libssh2_channel_write(ctx->x11_cur->channel,
                                ctx->x11_cur->relay_buf->data +
                                ctx->x11_cur->relay_buf->written,
                                ctx->x11_cur->wr_n));
                    if (ctx->x11_cur->channel->write_bytes > 0) {
                        ctx->x11_cur->relay_buf->written += ctx->x11_cur->channel->write_bytes;
                        ctx->x11_cur->ssh_ops++;
                    }
                    __sync_synchronize();
                    ctx->x11_cur->relay_buf->writing = 0;
                    corout_buffer_free(ctx->x11_cur->relay_buf);
                    ctx->x11_cur->relay_buf = NULL;
                }

                /* the 4a write may have yielded long enough for the runner to
                   drop xsock (or the socket to disconnect) */
                if (ctx->x11_cur->sock_done || !ctx->x11_cur->xsock->s)
                    continue;

                /* 4b. relay channel -> ming64x.exe (only when the buffer has room) */
                if (ssh2_channel_packet_data_len(ctx->x11_cur->channel, 0) > 0) {
                    ctx->x11_cur->relay_buf = ctx->x11_cur->xsock->s->bufwr;
                    ctx->x11_cur->rd_want = ctx->x11_cur->relay_buf->alloced -
                                            ctx->x11_cur->relay_buf->avail;
                    if (ctx->x11_cur->rd_want > ctx->x11_cur->relay_buf->alloced / 4) {
                        ctx->x11_cur->relay_buf->ref++;
                        ctx->x11_cur->relay_buf->reading = 1;
                        __sync_synchronize();
                        CALL_SOFT(libssh2_channel_read(ctx->x11_cur->channel,
                                    ctx->x11_cur->relay_buf->data +
                                    ctx->x11_cur->relay_buf->avail,
                                    ctx->x11_cur->rd_want));
                        if (ctx->x11_cur->channel->read_bytes > 0) {
                            ctx->x11_cur->relay_buf->avail += ctx->x11_cur->channel->read_bytes;
                            ctx->x11_cur->ssh_ops++;
                        }
                        __sync_synchronize();
                        ctx->x11_cur->relay_buf->reading = 0;
                        corout_buffer_free(ctx->x11_cur->relay_buf);
                        ctx->x11_cur->relay_buf = NULL;
                    }
                }

                /* wake the runner */
                if (!ctx->x11_cur->sock_done && ctx->x11_cur->ssh_ops)
                    corout_signal(state->o, ctx->x11_cur, X11_SIG);
            }
        } else {
            /* !s->x11_forwarding */
        }

        /* ---- PulseAudio: accept + relay reverse-forwarded connections ---- */
        if (s->audio_enabled && ctx->audio_listener) {
            /* Accept any connection the server has queued for us. */
            if (ssh2_list_first(&ctx->audio_listener->queue)) {
                CALL_SOFT(libssh2_channel_forward_accept(ctx->audio_listener));
                audio_open_cb(ctx->session, ctx->audio_listener->accepted_channel,
                              "127.0.0.1", 0, &ctx->session->abstract);
            }

            for (ctx->audio_rr = 0; ctx->audio_rr < AUDIO_MAX; ctx->audio_rr++) {
                ctx->audio_cur = ctx->audio[ctx->audio_rr];
                if (!ctx->audio_cur)
                    continue;

                /* 1. both sides finished: drop the connection */
                if (ctx->audio_cur->chan_done && ctx->audio_cur->sock_done) {
                    free(ctx->audio_cur);
                    ctx->audio[ctx->audio_rr] = NULL;
                    ctx->audio_cur = NULL;
                    continue;
                }

                /* 2. runner exited or daemon socket gone: close the channel */
                if (ctx->audio_cur->sock_done || !ctx->audio_cur->xsock->s) {
                    if (ctx->audio_cur->channel) {
                        CALL_SOFT(libssh2_channel_free(ctx->audio_cur->channel));
                        ctx->audio_cur->channel = NULL;
                    }
                    ctx->audio_cur->chan_done = 1;
                    continue;
                }

                /* 3. remote closed: close channel, wake runner to exit */
                if (!ctx->audio_cur->channel ||
                            ctx->audio_cur->channel->remote.eof ||
                            ctx->audio_cur->channel->remote.close) {
                    if (ctx->audio_cur->channel) {
                        CALL_SOFT(libssh2_channel_free(ctx->audio_cur->channel));
                        ctx->audio_cur->channel = NULL;
                    }
                    ctx->audio_cur->chan_done = 1;
                    if (!ctx->audio_cur->sock_done)
                        corout_signal(state->o, ctx->audio_cur, X11_SIG);
                    continue;
                }

                /* 4a. relay ming64x.exe -> channel */
                ctx->audio_cur->ssh_ops = 0;
                ctx->audio_cur->relay_buf = ctx->audio_cur->xsock->s->bufrd;
                ctx->audio_cur->wr_n = ctx->audio_cur->relay_buf->avail -
                                     ctx->audio_cur->relay_buf->written;
                if (ctx->audio_cur->wr_n > 0) {
                    ctx->audio_cur->relay_buf->ref++;
                    ctx->audio_cur->relay_buf->writing = 1;
                    __sync_synchronize();
                    CALL_SOFT(libssh2_channel_write(ctx->audio_cur->channel,
                                ctx->audio_cur->relay_buf->data +
                                ctx->audio_cur->relay_buf->written,
                                ctx->audio_cur->wr_n));
                    if (ctx->audio_cur->channel->write_bytes > 0) {
                        ctx->audio_cur->relay_buf->written += ctx->audio_cur->channel->write_bytes;
                        ctx->audio_cur->ssh_ops++;
                    }
                    __sync_synchronize();
                    ctx->audio_cur->relay_buf->writing = 0;
                    corout_buffer_free(ctx->audio_cur->relay_buf);
                    ctx->audio_cur->relay_buf = NULL;
                }

                /* the 4a write may have yielded long enough for the runner to
                   drop xsock (or the socket to disconnect) */
                if (ctx->audio_cur->sock_done || !ctx->audio_cur->xsock->s)
                    continue;

                /* 4b. relay channel -> ming64x.exe (only when the buffer has room) */
                if (ssh2_channel_packet_data_len(ctx->audio_cur->channel, 0) > 0) {
                    ctx->audio_cur->relay_buf = ctx->audio_cur->xsock->s->bufwr;
                    ctx->audio_cur->rd_want = ctx->audio_cur->relay_buf->alloced -
                                            ctx->audio_cur->relay_buf->avail;
                    if (ctx->audio_cur->rd_want > ctx->audio_cur->relay_buf->alloced / 4) {
                        ctx->audio_cur->relay_buf->ref++;
                        ctx->audio_cur->relay_buf->reading = 1;
                        __sync_synchronize();
                        CALL_SOFT(libssh2_channel_read(ctx->audio_cur->channel,
                                    ctx->audio_cur->relay_buf->data +
                                    ctx->audio_cur->relay_buf->avail,
                                    ctx->audio_cur->rd_want));
                        if (ctx->audio_cur->channel->read_bytes > 0) {
                            ctx->audio_cur->relay_buf->avail += ctx->audio_cur->channel->read_bytes;
                            ctx->audio_cur->ssh_ops++;
                        }
                        __sync_synchronize();
                        ctx->audio_cur->relay_buf->reading = 0;
                        corout_buffer_free(ctx->audio_cur->relay_buf);
                        ctx->audio_cur->relay_buf = NULL;
                    }
                }

                /* wake the runner */
                if (!ctx->audio_cur->sock_done && ctx->audio_cur->ssh_ops)
                    corout_signal(state->o, ctx->audio_cur, X11_SIG);
            }
        }

        /* Pop a fresh outbound chunk only once the previous one is fully
           written; wr_n/wr_off persist across yields so a partial write
           resumes with the remainder of the same chunk. */
        if (ctx->wr_n == 0)
            ctx->wr_n = byteq_pop(&s->out, ctx->wr_buf, sizeof ctx->wr_buf);
        while (ctx->wr_off < ctx->wr_n) {
            CALL_SOFT(libssh2_channel_write(ctx->channel,
                (const char *)ctx->wr_buf + ctx->wr_off,
                ctx->wr_n - ctx->wr_off));
            if (ctx->channel->write_bytes <= 0)
                break;
            ctx->wr_off += (size_t)ctx->channel->write_bytes;
        }
        if (ctx->wr_off >= ctx->wr_n)
            ctx->wr_off = ctx->wr_n = 0;

        if (InterlockedExchange(&s->resize_pending, 0))
            CALL_SOFT(libssh2_channel_request_pty_size_ex(ctx->channel,
                s->resize_cols, s->resize_rows, 0, 0));

        if (ssh2_channel_packet_data_len(ctx->channel, 0) > 0) {
            size_t space = byteq_space(&s->in);
            if (space > 0) {
                ctx->rd_want = space < sizeof ctx->rd_buf ? space
                                                          : sizeof ctx->rd_buf;
                CALL_SOFT(libssh2_channel_read(ctx->channel, ctx->rd_buf,
                    ctx->rd_want));
                if (ctx->channel->read_bytes > 0)
                    byteq_push(&s->in, (const unsigned char *)ctx->rd_buf,
                        (size_t)ctx->channel->read_bytes);
                continue;
            }
            /* s->in is full: fall through so the x11_runner coroutine and the
               UI-thread pump aren't starved while the terminal catches up */
        }

        if (ctx->channel->remote.eof || ctx->channel->remote.close)
            break;

        if (ctx->sock->s->bufrd->avail > ctx->sock->s->bufrd->written ||
            ctx->session->packet.writeidx > ctx->session->packet.readidx) {
            CALL_SOFT(ssh2_transport_read(ctx->session, 0));
            continue;
        }

        corout_read(ctx->sock);
        corout_wait_wakeable(state, SSH_POLL_MS);
        YIELD_();
    }

out:
    /* tear down any forwarded X11 connections before the session dies */
    {
        int i;
        for (i = 0; i < X11_MAX; i++) {
            if (ctx->x11[i]) {
                corout_kill(state->o, ctx->x11[i]);
                free(ctx->x11[i]);
                ctx->x11[i] = NULL;
                s->x11_open_count--;
            }
        }
    }
    /* tear down any forwarded PulseAudio connections before the session dies */
    {
        int i;
        for (i = 0; i < AUDIO_MAX; i++) {
            if (ctx->audio[i]) {
                corout_kill(state->o, ctx->audio[i]);
                free(ctx->audio[i]);
                ctx->audio[i] = NULL;
            }
        }
    }
    if (ctx->session && libssh2_session_last_errno(ctx->session)) {
        ssh_report_error(s, ctx->session, ctx->stage);
        if (ctx->fail_hint[0])
            set_display_error(s, "%s", ctx->fail_hint);
    }
    if (ctx->session) {
        ctx->tofree = ctx->session;
        ctx->session = NULL;
        CALL_SOFT(libssh2_session_free(ctx->tofree));
        ctx->tofree = NULL;
    }
    END();
}

static DWORD WINAPI
ssh_worker(LPVOID arg)
{
    ssh_session *s = (ssh_session *)arg;
    struct ssh_ctx ctx;
    struct corout *o;

    memset(&ctx, 0, sizeof ctx);
    ctx.s = s;
    ctx.stage = "connect";

    o = corout_alloc();
    corout_add(o, ssh_run, NULL, &ctx);
    corout_run(o);
    corout_free(o);

    InterlockedExchange(&s->running, 0);
    return 0;
}

/* ---- public API ---- */

void
ssh_session_init(ssh_session *s)
{
    memset(s, 0, sizeof *s);
    byteq_init(&s->in, SSH_IN_CAP, 1);
    byteq_init(&s->out, SSH_OUT_CAP, 1);
    InitializeCriticalSection(&s->prompt_lock);
    InitializeConditionVariable(&s->prompt_cv);
}

void
ssh_session_free(ssh_session *s)
{
    ssh_session_stop(s);
    byteq_free(&s->in);
    byteq_free(&s->out);
    DeleteCriticalSection(&s->prompt_lock);
}

void
ssh_session_start(ssh_session *s, const char *host,
    const char *username, const char *password, const int display_number,
    const int x11_forwarding, const int audio_enabled, const int audio_port,
    const unsigned char *audio_cookie)
{
    static int corout_ready = 0;

    if (s->running)
        return;

    if (!corout_ready) {
        corout_init();
        corout_ready = 1;
    }

    snprintf(s->host, sizeof s->host, "%s", host);
    snprintf(s->username, sizeof s->username, "%s", username);
    snprintf(s->password, sizeof s->password, "%s", password);
    s->display_number = display_number;
    s->x11_forwarding = x11_forwarding;
    s->audio_enabled = audio_enabled;
    s->audio_port = audio_port;
    if (audio_cookie)
        memcpy(s->audio_cookie, audio_cookie, sizeof s->audio_cookie);
    else
        memset(s->audio_cookie, 0, sizeof s->audio_cookie);
    s->display_error[0] = '\0';
    InterlockedExchange(&s->display_error_pending, 0);
    s->x11_open_count = 0;

    InterlockedExchange(&s->running, 1);
    s->thread = CreateThread(NULL, 0, ssh_worker, s, 0, NULL);
    if (!s->thread)
        InterlockedExchange(&s->running, 0);
}

void
ssh_session_stop(ssh_session *s)
{
    if (s->thread) {
        InterlockedExchange(&s->running, 0);
        EnterCriticalSection(&s->in.lock);
        s->in.closed = 1;
        WakeAllConditionVariable(&s->in.not_full);
        LeaveCriticalSection(&s->in.lock);
        EnterCriticalSection(&s->out.lock);
        s->out.closed = 1;
        WakeAllConditionVariable(&s->out.not_full);
        LeaveCriticalSection(&s->out.lock);
        InterlockedExchange(&s->prompt_pending, 0);
        WakeAllConditionVariable(&s->prompt_cv);
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
        s->in.closed = 0;   /* allow a later reconnect */
        s->out.closed = 0;
    }
    SecureZeroMemory(s->password, sizeof s->password);
}

int
ssh_session_is_active(const ssh_session *s)
{
    return s->running != 0;
}

int
ssh_x11_open_count(const ssh_session *s)
{
    return (int)s->x11_open_count;
}

void
ssh_send(ssh_session *s, const char *bytes, size_t n)
{
    int pushed = 0;
    while (s->running && n > 0) {
        size_t chunk = n < SSH_SEND_CHUNK ? n : SSH_SEND_CHUNK;
        byteq_push(&s->out, (const unsigned char*)bytes, chunk);
        bytes += chunk;
        n -= chunk;
        pushed = 1;
    }
    if (pushed) {
        HANDLE h = s->iocp_handle;
        if (h)
            PostQueuedCompletionStatus(h, 0, (ULONG_PTR)0, NULL);
    }
}

void
ssh_request_resize(ssh_session *s, const int cols, const int rows)
{
    if (!s->running)
        return;
    s->resize_cols = cols;
    s->resize_rows = rows;
    InterlockedExchange(&s->resize_pending, 1);
}

void
ssh_paste_clipboard(ssh_session *s)
{
    HGLOBAL mem;
    LPCWSTR wstr;
    char *utf8;
    int utf8len;

    if (!ssh_session_is_active(s))
        return;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
        return;
    mem = GetClipboardData(CF_UNICODETEXT);
    if (mem) {
        wstr = (LPCWSTR)GlobalLock(mem);
        if (wstr) {
            utf8len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
            if (utf8len > 1) {
                utf8 = (char*)malloc((size_t)utf8len);
                if (utf8) {
                    char *dst = utf8;
                    size_t i, n = (size_t)(utf8len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, utf8len, NULL, NULL);
                    /* Collapse CRLF -> LF so a Windows paste doesn't double
                       each newline against the remote pty's ICRNL. */
                    for (i = 0; i < n; ++i) {
                        if (utf8[i] == '\r' && i + 1 < n && utf8[i + 1] == '\n')
                            continue;
                        *dst++ = utf8[i];
                    }
                    ssh_send(s, utf8, (size_t)(dst - utf8));
                    free(utf8);
                }
            }
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
}

int
ssh_hostkey_pending(const ssh_session *s)
{
    return s->prompt_pending != 0;
}

const char *
ssh_hostkey_host(const ssh_session *s)
{
    return s->prompt_host;
}

const char *
ssh_hostkey_keytype(const ssh_session *s)
{
    return s->prompt_keytype;
}

const char *
ssh_hostkey_fingerprint(const ssh_session *s)
{
    return s->prompt_fingerprint;
}

void
ssh_hostkey_answer(ssh_session *s, const int accept)
{
    InterlockedExchange(&s->prompt_answer, accept ? 1 : 0);
    InterlockedExchange(&s->prompt_pending, 0);
    WakeAllConditionVariable(&s->prompt_cv);
}

/* ---- UI-thread pump (drain channel output into the terminal) ---- */

static wchar_t
utf8_decode_seq(const unsigned char *p, const int len)
{
    unsigned int cp;

    if (len == 2)
        cp = ((unsigned int)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if (len == 3)
        cp = ((unsigned int)(p[0] & 0x0F) << 12)
           | ((unsigned int)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else
        cp = ((unsigned int)(p[0] & 0x07) << 18)
           | ((unsigned int)(p[1] & 0x3F) << 12)
           | ((unsigned int)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);

    return cp > 0xFFFF ? L'?' : (wchar_t)cp;
}

void
ssh_pump(ssh_session *s, struct terminal *t)
{
    unsigned char buf[16384];
    wchar_t wide[32768];
    size_t n = byteq_pop(&s->in, buf, sizeof buf);
    size_t i = 0, wi = 0;

    if (n > 0) {
        HANDLE h = s->iocp_handle;
        if (h)
            PostQueuedCompletionStatus(h, 0, (ULONG_PTR)0, NULL);
    }

    if (n == 0 && s->utf8_npending == 0)
        return;

    while (i < n && wi < sizeof(wide) / sizeof(wide[0])) {
        unsigned char b = buf[i];

        if (s->utf8_npending == 0) {
            if (b < 0x80)
                wide[wi++] = (wchar_t)b;
            else if ((b & 0xE0) == 0xC0) {
                s->utf8_expected = 2;
                s->utf8_pending[s->utf8_npending++] = b;
            }
            else if ((b & 0xF0) == 0xE0) {
                s->utf8_expected = 3;
                s->utf8_pending[s->utf8_npending++] = b;
            }
            else if ((b & 0xF8) == 0xF0) {
                s->utf8_expected = 4;
                s->utf8_pending[s->utf8_npending++] = b;
            }
            else
                wide[wi++] = L'?';
            ++i;
        }
        else {
            if ((b & 0xC0) != 0x80) {
                wide[wi++] = L'?';       /* malformed continuation */
                s->utf8_npending = 0;    /* reprocess b as a lead byte */
            }
            else {
                s->utf8_pending[s->utf8_npending++] = b;
                if (s->utf8_npending == s->utf8_expected) {
                    wide[wi++] = utf8_decode_seq(s->utf8_pending,
                        s->utf8_expected);
                    s->utf8_npending = 0;
                }
                ++i;
            }
        }
    }

    if (wi > 0) {
        wide[wi] = L'\0';
        terminal_print(t, wide);
    }
}
