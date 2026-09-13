#ifndef SSH_H
#define SSH_H

#include <stddef.h>
#include <winsock2.h>
#include <windows.h>

struct terminal;

/* single-producer / single-consumer byte ring, guarded by a critical section */
typedef struct byteq {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE not_full;
    unsigned char *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t n;
    int block;                       /* 1 = block producer when full, 0 = drop-oldest */
    volatile int closed;             /* wake blocked producers on stop */
} byteq;

typedef struct ssh_session {
    char host[128];
    char username[128];
    char password[128];
    int display_number;              /* :0 .. :12, set before start */
    char display_error[256];         /* DISPLAY setup error text for the UI */
    volatile LONG display_error_pending;  /* 1 = display_error is valid */

    HANDLE thread;
    volatile LONG running;

    byteq in;                       /* channel -> terminal (UTF-8 bytes) */
    byteq out;                      /* terminal -> channel (keystrokes) */

    volatile LONG resize_pending;   /* flag handoff: cols/rows written first */
    int resize_cols;
    int resize_rows;

    /* host-key verification prompt (worker blocks, UI answers) */
    CRITICAL_SECTION prompt_lock;
    CONDITION_VARIABLE prompt_cv;
    volatile LONG prompt_pending;   /* 1 = UI should show the dialog */
    volatile LONG prompt_answer;    /* 1 = trust & save, 0 = reject */
    char prompt_host[128];
    char prompt_keytype[32];
    char prompt_fingerprint[128];   /* "SHA256:base64..." */

    /* incremental UTF-8 decode state (UI thread) */
    unsigned char utf8_pending[4];
    int utf8_npending;
    int utf8_expected;
} ssh_session;

void ssh_session_init(ssh_session *s);
void ssh_session_free(ssh_session *s);
void ssh_session_start(ssh_session *s, const char *host,
    const char *username, const char *password, int display_number);
void ssh_session_stop(ssh_session *s);
int  ssh_session_is_active(const ssh_session *s);
void ssh_pump(ssh_session *s, struct terminal *t);
void ssh_send(ssh_session *s, const char *bytes, size_t n);
void ssh_request_resize(ssh_session *s, int cols, int rows);
void ssh_paste_clipboard(ssh_session *s);
int  ssh_hostkey_pending(const ssh_session *s);
const char *ssh_hostkey_host(const ssh_session *s);
const char *ssh_hostkey_keytype(const ssh_session *s);
const char *ssh_hostkey_fingerprint(const ssh_session *s);
void ssh_hostkey_answer(ssh_session *s, int accept);

#endif /* SSH_H */
