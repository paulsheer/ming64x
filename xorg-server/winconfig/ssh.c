#include "ssh.h"

#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "libssh2.h"
#include "terminal.h"

#define SSH_PORT       "22"
#define SSH_IN_CAP     (64 * 1024)
#define SSH_OUT_CAP    (16 * 1024)
#define SSH_IO_TIMEOUT 200   /* ms; bounds each blocking libssh2 call */
#define SSH_CONNECT_TIMEOUT 15000  /* ms; handshake/auth/channel-open */
#define SSH_SEND_CHUNK 8192

/* ---- byte queue ---- */

static void
byteq_init(byteq *q, size_t cap, int block)
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
byteq_pop(byteq *q, unsigned char *dst, size_t max)
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
b64_encode(const unsigned char *in, size_t len, char *out, size_t cap)
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
hostkey_type_name(int type)
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
known_hosts_path(char *buf, size_t cap)
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

/* Write the local endpoint IP address of sock as text (IPv4 dotted-quad
   or IPv6).  Returns 0 on success, nonzero if the address can't be fetched. */
static int
get_local_ip(ssh_session *s, SOCKET sock, char *out, size_t outsz)
{
    struct sockaddr_storage ss;
    int len = sizeof ss;

    if (outsz == 0)
        return -1;
    out[0] = '\0';

    if (getsockname(sock, (struct sockaddr *)&ss, &len) != 0) {
        int err = WSAGetLastError();
        set_display_error(s, "Could not determine local IP (WSA error %d)",
            err);
        return -1;
    }

    if (ss.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        if (!inet_ntop(AF_INET, &sin->sin_addr, out, outsz)) {
            set_display_error(s, "Could not format local IP address");
            return -1;
        }
        return 0;
    }
    if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        if (!inet_ntop(AF_INET6, &sin6->sin6_addr, out, outsz)) {
            set_display_error(s, "Could not format local IP address");
            return -1;
        }
        return 0;
    }
    set_display_error(s, "Unsupported address family %d", (int)ss.ss_family);
    return -1;
}

static DWORD WINAPI
ssh_worker(LPVOID arg)
{
    ssh_session *s = (ssh_session*)arg;
    LIBSSH2_SESSION *session = NULL;
    LIBSSH2_CHANNEL *channel = NULL;
    struct addrinfo hints, *res = NULL, *ai;
    SOCKET sock = INVALID_SOCKET;
    char buf[8192];
    unsigned char obuf[8192];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(s->host, SSH_PORT, &hints, &res) != 0) {
        ssh_report(s, "connect: host lookup failed\r\n");
        set_display_error(s, "Could not resolve host \"%s\"", s->host);
        goto out;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET)
            continue;
        if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (sock == INVALID_SOCKET) {
        ssh_report(s, "connect: connection failed\r\n");
        set_display_error(s, "Could not connect to \"%s\"", s->host);
        goto out;
    }

    session = libssh2_session_init();
    if (!session) {
        ssh_report(s, "session: init failed\r\n");
        goto out;
    }
    libssh2_session_set_timeout(session, SSH_CONNECT_TIMEOUT);

    if (libssh2_session_handshake(session, sock) != 0) {
        char *errmsg = NULL;
        int errlen = 0;
        int err = libssh2_session_last_error(session, &errmsg, &errlen, 0);
        char line[256];
        snprintf(line, sizeof line,
            "session: handshake failed (%d: %.*s)\r\n",
            err, errlen, errmsg ? errmsg : "");
        ssh_report(s, line);
        set_display_error(s, "SSH handshake failed (%d: %.*s)",
            err, errlen, errmsg ? errmsg : "");
        goto out;
    }

    if (!hostkey_verify(s, session))
        goto out;

    if (libssh2_userauth_password(session, s->username, s->password) != 0) {
        char *errmsg = NULL;
        int errlen = 0;
        int err = libssh2_session_last_error(session, &errmsg, &errlen, 0);
        char line[256];
        snprintf(line, sizeof line,
            "auth: password rejected (%d: %.*s)\r\n",
            err, errlen, errmsg ? errmsg : "");
        ssh_report(s, line);
        set_display_error(s, "Authentication failed (%d: %.*s)",
            err, errlen, errmsg ? errmsg : "");
        goto out;
    }

    channel = libssh2_channel_open_session(session);
    if (!channel) {
        ssh_report(s, "channel: open failed\r\n");
        goto out;
    }

    {
        char lip[64];
        int gip = get_local_ip(s, sock, lip, sizeof lip);
        if (gip != 0) {
            /* get_local_ip already recorded the reason in s->display_error */
        } else {
            char disp[80];
            int rc;
            snprintf(disp, sizeof disp, "%s:%d.0", lip, s->display_number);
            rc = libssh2_channel_setenv_ex(channel, "DISPLAY", 7, disp,
                (unsigned int)strlen(disp));
            if (rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED)
                set_display_error(s, "DISPLAY rejected by server "
                    "(add 'AcceptEnv DISPLAY' to sshd_config)");
            else if (rc != 0)
                set_display_error(s, "Could not set DISPLAY "
                    "(libssh2 error %d)", rc);
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
        int w = s->resize_cols > 0 ? s->resize_cols : 80;
        int h = s->resize_rows > 0 ? s->resize_rows : 24;

        if (libssh2_channel_request_pty_ex(channel, "xterm", 5,
                (const char *)raw_modes, sizeof raw_modes,
                w, h, 0, 0) != 0 ||
            libssh2_channel_shell(channel) != 0) {
            ssh_report(s, "channel: pty/shell failed\r\n");
            set_display_error(s, "Could not start remote shell");
            goto out;
        }
    }

    ssh_report(s, "connected\r\n");

    libssh2_session_set_timeout(session, SSH_IO_TIMEOUT);

    while (s->running) {
        size_t n = byteq_pop(&s->out, obuf, sizeof obuf);
        if (n > 0) {
            size_t off = 0;
            while (off < n && s->running) {
                ssize_t w = libssh2_channel_write(channel,
                    (const char*)obuf + off, n - off);
                if (w > 0) {
                    off += (size_t)w;
                } else if (w == LIBSSH2_ERROR_EAGAIN ||
                           w == LIBSSH2_ERROR_TIMEOUT) {
                    /* session timeout (200 ms) paces the retry */
                } else {
                    break;
                }
            }
        }

        if (InterlockedExchange(&s->resize_pending, 0)) {
            libssh2_channel_request_pty_size_ex(channel,
                s->resize_cols, s->resize_rows, 0, 0);
        }

        {
            size_t space = byteq_space(&s->in);
            if (space > 0) {
                size_t want = space < sizeof buf ? space : sizeof buf;
                ssize_t rc = libssh2_channel_read(channel, buf, (size_t)want);
                if (rc > 0) {
                    byteq_push(&s->in, (const unsigned char*)buf, (size_t)rc);
                }
                else if (rc == 0)
                    break;   /* remote closed the channel */
                else if (rc != LIBSSH2_ERROR_EAGAIN && rc != LIBSSH2_ERROR_TIMEOUT)
                    break;   /* genuine error */
            } else {
                Sleep(1);    /* in full: pause read -> SSH flow control */
            }
        }
    }

out:
    if (channel)
        libssh2_channel_free(channel);
    if (session) {
        libssh2_session_disconnect_ex(session, SSH_DISCONNECT_BY_APPLICATION,
            "closing", "");
        libssh2_session_free(session);
    }
    if (sock != INVALID_SOCKET)
        closesocket(sock);

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
    const char *username, const char *password, int display_number)
{
    static int wsa_ready = 0;

    if (s->running)
        return;

    if (!wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return;
        wsa_ready = 1;
    }

    snprintf(s->host, sizeof s->host, "%s", host);
    snprintf(s->username, sizeof s->username, "%s", username);
    snprintf(s->password, sizeof s->password, "%s", password);
    s->display_number = display_number;
    s->display_error[0] = '\0';
    InterlockedExchange(&s->display_error_pending, 0);

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

void
ssh_send(ssh_session *s, const char *bytes, size_t n)
{
    while (s->running && n > 0) {
        size_t chunk = n < SSH_SEND_CHUNK ? n : SSH_SEND_CHUNK;
        byteq_push(&s->out, (const unsigned char*)bytes, chunk);
        bytes += chunk;
        n -= chunk;
    }
}

void
ssh_request_resize(ssh_session *s, int cols, int rows)
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
ssh_hostkey_answer(ssh_session *s, int accept)
{
    InterlockedExchange(&s->prompt_answer, accept ? 1 : 0);
    InterlockedExchange(&s->prompt_pending, 0);
    WakeAllConditionVariable(&s->prompt_cv);
}

/* ---- UI-thread pump (drain channel output into the terminal) ---- */

static wchar_t
utf8_decode_seq(const unsigned char *p, int len)
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
    unsigned char buf[4096];
    wchar_t wide[8192];
    size_t n = byteq_pop(&s->in, buf, sizeof buf);
    size_t i = 0, wi = 0;

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
