/*
 * x11cliptest.c - Linux clipboard test driver.
 *
 * Connects to win32cliptest.exe via TCP, sends test requests, receives
 * results, writes images to clip-results/, and produces results.txt with
 * transparency analysis.
 *
 * Build:  gcc -o x11cliptest x11cliptest.c -lm
 * Usage:  ./x11cliptest -host <addr> [-start N] [-end N] [-test N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>

#include <xcb/xcb.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "dib.h"

#include "cliptest.h"

#define DEFAULT_PORT       21407
#define INPUT_DIR          "input"
#define OUTPUT_DIR         "clip-results"
#define RESULTS_FILE       "results.txt"
#define MAX_IMAGE_SIZE     (128 * 1024 * 1024)
#define TEST_TIMEOUT_SEC   30

/* Check elapsed time since test started.  Returns 1 if timed out. */
static int timed_out(time_t start)
{
    return (time(NULL) - start) > TEST_TIMEOUT_SEC;
}

/* ---------- X11 running window ---------- */
static xcb_connection_t *g_x11_conn = NULL;
static xcb_window_t      g_x11_win  = XCB_NONE;

static int create_running_window(const char *display_name)
{
    g_x11_conn = xcb_connect(display_name, NULL);
    if (xcb_connection_has_error(g_x11_conn)) {
        g_x11_conn = NULL;
        return -1;
    }

    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup(g_x11_conn)).data;
    g_x11_win = xcb_generate_id(g_x11_conn);

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t vals[] = { screen->white_pixel,
                        XCB_EVENT_MASK_EXPOSURE |
                        XCB_EVENT_MASK_PROPERTY_CHANGE };

    xcb_create_window(g_x11_conn, XCB_COPY_FROM_PARENT, g_x11_win,
                      screen->root, 0, 0, 100, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, mask, vals);

    const char *title = "Running";
    xcb_change_property(g_x11_conn, XCB_PROP_MODE_REPLACE, g_x11_win,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        strlen(title), title);

    /* Open font for drawing text inside the window */
    xcb_font_t font = xcb_generate_id(g_x11_conn);
    xcb_open_font(g_x11_conn, font, strlen("fixed"), "fixed");

    xcb_gcontext_t gc = xcb_generate_id(g_x11_conn);
    uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_FONT;
    uint32_t gc_vals[] = { screen->black_pixel, font };
    xcb_create_gc(g_x11_conn, gc, g_x11_win, gc_mask, gc_vals);

    /* Query text width to center "Running" */
    xcb_char2b_t run_chars[7];
    for (int i = 0; i < 7; i++) {
        run_chars[i].byte1 = 0;
        run_chars[i].byte2 = "Running"[i];
    }
    xcb_query_text_extents_cookie_t te_cookie =
        xcb_query_text_extents(g_x11_conn, font, 7, run_chars);
    xcb_query_text_extents_reply_t *te_reply =
        xcb_query_text_extents_reply(g_x11_conn, te_cookie, NULL);

    int text_x = 15;  /* fallback */
    if (te_reply) {
        text_x = (100 - (int)te_reply->overall_width) / 2;
        if (text_x < 2) text_x = 2;
        free(te_reply);
    }

    xcb_image_text_8(g_x11_conn, 7, g_x11_win, gc, text_x, 55, "Running");
    xcb_close_font(g_x11_conn, font);

    xcb_map_window(g_x11_conn, g_x11_win);
    xcb_flush(g_x11_conn);

    return 0;
}

static void destroy_running_window(void)
{
    if (g_x11_win != XCB_NONE && g_x11_conn) {
        xcb_destroy_window(g_x11_conn, g_x11_win);
        xcb_flush(g_x11_conn);
        g_x11_win = XCB_NONE;
    }
    if (g_x11_conn) {
        xcb_disconnect(g_x11_conn);
        g_x11_conn = NULL;
    }
}

/* ---------- logging ---------- */
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    va_end(ap);
}

/* ---------- socket helpers ---------- */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---------- test generation ---------- */

static const uint32_t from_formats_no_dib[] = {
    CLIPTEST_FORMAT_PNG, CLIPTEST_FORMAT_BMP, CLIPTEST_FORMAT_GIF,
    CLIPTEST_FORMAT_JPEG
};
static const int num_from_formats_no_dib = 4;

static const uint32_t from_formats_win32[] = {
    CLIPTEST_FORMAT_PNG, CLIPTEST_FORMAT_BMP, CLIPTEST_FORMAT_GIF,
    CLIPTEST_FORMAT_JPEG, CLIPTEST_FORMAT_DIB, CLIPTEST_FORMAT_DIBV5
};
static const int num_from_formats_win32 = 6;

static const uint32_t to_formats_win32[] = {
    CLIPTEST_FORMAT_DIB, CLIPTEST_FORMAT_DIBV5
};
static const int num_to_formats_win32 = 2;

static const uint32_t to_formats_x11[] = {
    CLIPTEST_FORMAT_PNG, CLIPTEST_FORMAT_BMP, CLIPTEST_FORMAT_GIF,
    CLIPTEST_FORMAT_JPEG
};
static const int num_to_formats_x11 = 4;

static const uint32_t directions[] = {
    CLIPTEST_WIN32_TO_X11, CLIPTEST_X11_TO_WIN32
};

static const uint32_t hdrops[] = {
    CLIPTEST_HDROP_YES, CLIPTEST_HDROP_NO
};

static const uint32_t transparencies[] = {
    CLIPTEST_TRANSPARENCY_YES, CLIPTEST_TRANSPARENCY_NO
};

struct size_entry {
    const char *name;
    int width;
};

static const struct size_entry sizes[] = {
    { "small",  200  },
    { "medium", 900  },
    { "large",  3600 },
};

/*
 * Generate the test array.  Caller must free each cliptest entry.
 * Returns number of tests, stores pointer in *out.
 */
static int generate_tests(struct cliptest **out)
{
    int capacity = 512;
    int count = 0;
    struct cliptest *tests = (struct cliptest *)malloc(
        (size_t)capacity * sizeof(struct cliptest));
    if (!tests) return 0;

    for (int di = 0; di < 2; di++) {
        uint32_t dir = directions[di];
        int is_win32_sender = (dir == CLIPTEST_WIN32_TO_X11);

        const uint32_t *ffmts = is_win32_sender ? from_formats_win32
                                                 : from_formats_no_dib;
        int n_ff = is_win32_sender ? num_from_formats_win32 : num_from_formats_no_dib;

        const uint32_t *tfmts = is_win32_sender ? to_formats_x11
                                                 : to_formats_win32;
        int n_tf = is_win32_sender ? num_to_formats_x11 : num_to_formats_win32;

        for (int fi = 0; fi < n_ff; fi++) {
        for (int ti = 0; ti < n_tf; ti++) {
        for (int hi = 0; hi < 2; hi++) {
        for (int si = 0; si < 3; si++) {
        for (int xi = 0; xi < 2; xi++) {

            /* HDROP makes no sense for X11_TO_WIN32 (X11 can't drop
               files onto the Windows filesystem). */
            if (dir == CLIPTEST_X11_TO_WIN32 &&
                hdrops[hi] == CLIPTEST_HDROP_YES)
                continue;

            /* DIB/DIBV5 from-formats make no sense with HDROP_YES
               (X11 has no DIB selection target to put data on). */
            if (dir == CLIPTEST_WIN32_TO_X11 &&
                hdrops[hi] == CLIPTEST_HDROP_YES &&
                (ffmts[fi] == CLIPTEST_FORMAT_DIB ||
                 ffmts[fi] == CLIPTEST_FORMAT_DIBV5))
                continue;

            if (count >= capacity) {
                capacity *= 2;
                tests = (struct cliptest *)realloc(tests,
                    (size_t)capacity * sizeof(struct cliptest));
                if (!tests) return 0;
            }

            tests[count].direction     = dir;
            tests[count].from_format   = ffmts[fi];
            tests[count].to_format     = tfmts[ti];
            tests[count].hdrop         = hdrops[hi];
            tests[count].width         = sizes[si].width;
            tests[count].height        = sizes[si].width;
            tests[count].transparency  = transparencies[xi];
            count++;
        }}}}}}

    *out = tests;
    return count;
}

/* ---------- transparency analysis ---------- */

/*
 * Return the number of transparent pixels (alpha < 128) in an image file.
 */
static int count_transparent_pixels(const char *path)
{
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return -1;

    int count = 0;
    int total = w * h;
    for (int i = 0; i < total; i++) {
        if (pixels[i * 4 + 3] < 128)
            count++;
    }
    stbi_image_free(pixels);
    return count;
}

/*
 * Expected transparent pixels for a circle mask of diameter = width
 * centered in a square image: area outside the circle.
 *   outside = w² - π(w/2)² = w²(1 - π/4)
 */
static int expected_transparent(int width, int has_transparency)
{
    if (!has_transparency) return 0;
    double w = (double)width;
    return (int)(w * w * (1.0 - M_PI / 4.0));
}

/*
 * Return the number of gray pixels (R,G,B all within threshold of 128)
 * in an image file.  Used for BMP/JPEG where alpha is lost but the
 * transparent area was composited onto #808080 gray.
 */
static int count_gray_pixels(const char *path)
{
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 3);
    if (!pixels) return -1;

    const int threshold = 10;
    int count = 0;
    int total = w * h;
    for (int i = 0; i < total; i++) {
        int r = pixels[i * 3 + 0];
        int g = pixels[i * 3 + 1];
        int b = pixels[i * 3 + 2];
        if (abs(r - 128) <= threshold &&
            abs(g - 128) <= threshold &&
            abs(b - 128) <= threshold)
            count++;
    }
    stbi_image_free(pixels);
    return count;
}

/*
 * Check whether actual count is within ±5% of expected.
 */
static int within_tolerance(int actual, int expected, int width)
{
    if (expected == 0) {
        /* Allow up to 2% of total pixels as fudge for natural images
           that coincidentally have gray-ish or near-transparent pixels */
        int max_fudge = (int)(0.02 * (double)(width * width));
        return (actual <= max_fudge);
    }
    double tolerance = 0.05 * (double)(width * width);
    double diff = fabs((double)(actual - expected));
    return (diff <= tolerance);
}

/* ---------- md5sum ---------- */

static int md5sum_file(const char *path, char *hash_out, size_t hash_sz)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "md5sum %s 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char *got = fgets(hash_out, (int)hash_sz, fp);
    pclose(fp);
    if (!got) return -1;
    /* md5sum outputs "hash  filename" — extract just the hash */
    char *sp = strchr(hash_out, ' ');
    if (sp) *sp = '\0';
    return 0;
}

/* ---------- X11 selection infrastructure ---------- */

/*
 * Atoms that match VcXsrv's winclipboard interned atoms (thread.c:188-199).
 * Must be the exact same strings so VcXsrv recognizes them.
 */
static struct {
    xcb_atom_t clipboard;
    xcb_atom_t targets;
    xcb_atom_t incr;
    xcb_atom_t png;
    xcb_atom_t bmp;
    xcb_atom_t gif;
    xcb_atom_t jpeg;
    xcb_atom_t property;   /* local property for data transfer */
    xcb_atom_t debug_on;
    xcb_atom_t debug_off;
    xcb_atom_t wm_name;
} xa;

/* VcXsrv's iWindow (discovered during CLIPBOARD owner poll) */
static xcb_window_t g_vcxsrv_win = XCB_NONE;

static xcb_atom_t intern_atom(xcb_connection_t *conn, const char *name)
{
    xcb_intern_atom_cookie_t ck = xcb_intern_atom(conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static void intern_all_atoms(xcb_connection_t *conn)
{
    xa.clipboard = intern_atom(conn, "CLIPBOARD");
    xa.targets   = intern_atom(conn, "TARGETS");
    xa.incr      = intern_atom(conn, "INCR");
    xa.png       = intern_atom(conn, "image/png");
    xa.bmp       = intern_atom(conn, "image/bmp");
    xa.gif       = intern_atom(conn, "image/gif");
    xa.jpeg      = intern_atom(conn, "image/jpeg");
    xa.property  = intern_atom(conn, "CLIPTEST_PROPERTY");
    xa.debug_on  = intern_atom(conn, "CLIPTEST_DBG_ON");
    xa.debug_off = intern_atom(conn, "CLIPTEST_DBG_OFF");
    xa.wm_name   = intern_atom(conn, "WM_NAME");
}

/* Map internal format enum → X11 atom */
static xcb_atom_t format_to_atom(uint32_t fmt)
{
    switch (fmt) {
    case CLIPTEST_FORMAT_PNG:  return xa.png;
    case CLIPTEST_FORMAT_BMP:  return xa.bmp;
    case CLIPTEST_FORMAT_GIF:  return xa.gif;
    case CLIPTEST_FORMAT_JPEG: return xa.jpeg;
    default:                   return XCB_ATOM_NONE;
    }
}

/* Map X11 atom → internal format enum */
static uint32_t atom_to_format(xcb_atom_t atom)
{
    if (atom == xa.png)  return CLIPTEST_FORMAT_PNG;
    if (atom == xa.bmp)  return CLIPTEST_FORMAT_BMP;
    if (atom == xa.gif)  return CLIPTEST_FORMAT_GIF;
    if (atom == xa.jpeg) return CLIPTEST_FORMAT_JPEG;
    return 0;
}

/* Get a readable name for an atom (for debug logging) */
static const char *dbg_atom_name(xcb_connection_t *c, xcb_atom_t atom)
{
    static char buf[64];
    if (atom == XCB_ATOM_NONE)          return "NONE";
    if (atom == XCB_ATOM_STRING)        return "STRING";
    if (atom == XCB_ATOM_ATOM)          return "ATOM";
    if (atom == xa.clipboard)           return "CLIPBOARD";
    if (atom == xa.targets)             return "TARGETS";
    if (atom == xa.incr)                return "INCR";
    if (atom == xa.png)                 return "image/png";
    if (atom == xa.bmp)                 return "image/bmp";
    if (atom == xa.gif)                 return "image/gif";
    if (atom == xa.jpeg)                return "image/jpeg";
    if (atom == xa.property)            return "CLIPTEST_PROPERTY";
    xcb_get_atom_name_cookie_t ck = xcb_get_atom_name(c, atom);
    xcb_get_atom_name_reply_t *rp = xcb_get_atom_name_reply(c, ck, NULL);
    if (rp) {
        int len = xcb_get_atom_name_name_length(rp);
        if (len > 60) len = 60;
        memcpy(buf, xcb_get_atom_name_name(rp), len);
        buf[len] = '\0';
        free(rp);
    } else {
        snprintf(buf, sizeof(buf), "atom#%u", (unsigned)atom);
    }
    return buf;
}

/*
 * Selection-owner state (X11_TO_WIN32 direction).
 * When Linux owns CLIPBOARD, these hold the image bytes we serve to VcXsrv.
 */
static unsigned char *g_sel_data = NULL;
static size_t        g_sel_len  = 0;
static uint32_t      g_sel_fmt  = 0;
static int           g_sel_delivered = 0;
static int           g_sel_lost      = 0;  /* CLIPBOARD taken by another owner */

/*
 * Selection-requester state (WIN32_TO_X11 direction).
 * INCR accumulation and completion tracking.
 */
static unsigned char *g_recv_incr     = NULL;
static size_t        g_recv_incr_size = 0;
static int           g_recv_done      = 0;

/* ---------- INCR send ---------- */

/*
 * Send image data to a requestor via ICCCM INCR protocol.
 * Mirrors winSendImageIncr in xevents.c.
 */
static int send_incr(xcb_connection_t *conn, xcb_window_t requestor,
                     xcb_atom_t property, xcb_atom_t selection,
                     xcb_atom_t target, const void *data, size_t len)
{
    uint32_t max_chunk = (uint32_t)xcb_get_maximum_request_length(conn) * 4 - 24;
    if (max_chunk > 262144) max_chunk = 262144;

    /* Step 1: INCR start */
    uint32_t lower = (uint32_t)len;
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, requestor, property,
                        xa.incr, 32, 1, &lower);
    xcb_selection_notify_event_t sn = { 0 };
    sn.response_type = XCB_SELECTION_NOTIFY;
    sn.requestor = requestor;
    sn.selection = selection;
    sn.target   = target;
    sn.property = property;
    sn.time     = XCB_CURRENT_TIME;
    xcb_send_event(conn, 0, requestor, 0, (const char *)&sn);
    xcb_flush(conn);

    /* Step 2: send chunks */
    const unsigned char *src = (const unsigned char *)data;
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_chunk) chunk = max_chunk;

        /* Wait for property delete (requestor consumed previous chunk) */
        for (;;) {
            xcb_get_property_cookie_t ck =
                xcb_get_property(conn, 0, requestor, property, XCB_ATOM_ANY, 0, 0);
            xcb_get_property_reply_t *rp =
                xcb_get_property_reply(conn, ck, NULL);
            int deleted = !rp || rp->type == XCB_ATOM_NONE;
            free(rp);
            if (deleted) break;
            usleep(20000);
        }

        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, requestor,
                            property, target, 8, (uint32_t)chunk, src + offset);
        xcb_flush(conn);
        offset += chunk;
    }

    /* Step 3: zero-length final chunk */
    for (;;) {
        xcb_get_property_cookie_t ck =
            xcb_get_property(conn, 0, requestor, property, XCB_ATOM_ANY, 0, 0);
        xcb_get_property_reply_t *rp =
            xcb_get_property_reply(conn, ck, NULL);
        int deleted = !rp || rp->type == XCB_ATOM_NONE;
        free(rp);
        if (deleted) break;
        usleep(20000);
    }
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, requestor,
                        property, target, 8, 0, NULL);
    xcb_flush(conn);
    return 1;
}

/* ---------- X11 event handling ---------- */

static int handle_selection_request(xcb_connection_t *conn,
                                    xcb_selection_request_event_t *ev)
{
#ifdef VERBOSE
    log_msg(" [SR: sel=0x%x tgt=0x%x req=0x%x prop=0x%x]",
            (unsigned)ev->selection, (unsigned)ev->target,
            (unsigned)ev->requestor, (unsigned)ev->property);
#endif
    if (ev->target == xa.targets) {
#ifdef VERBOSE
        log_msg(" [TARGETS prop=%s]", dbg_atom_name(conn, ev->property));
#endif
        /* Respond with the targets we support based on g_sel_fmt */
        xcb_atom_t targets[8];
        int n = 0;
        targets[n++] = xa.targets;
        /* Advertise all image formats */
        targets[n++] = xa.png;
        targets[n++] = xa.bmp;
        targets[n++] = xa.gif;
        targets[n++] = xa.jpeg;

        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, ev->requestor,
                            ev->property, XCB_ATOM_ATOM, 32, n, targets);

        xcb_selection_notify_event_t sn = { 0 };
        sn.response_type = XCB_SELECTION_NOTIFY;
        sn.requestor = ev->requestor;
        sn.selection = ev->selection;
        sn.target   = ev->target;
        sn.property = ev->property;
        sn.time     = ev->time;
        xcb_send_event(conn, 0, ev->requestor, 0, (const char *)&sn);
        xcb_flush(conn);
        return 1;
    }

    if (ev->target == xa.png || ev->target == xa.bmp ||
        ev->target == xa.gif || ev->target == xa.jpeg) {
#ifdef VERBOSE
        log_msg(" [IMAGE tgt=%s]", dbg_atom_name(conn, ev->target));
#endif
        if (!g_sel_data || g_sel_len == 0) {
            /* No data — refuse */
#ifdef VERBOSE
            log_msg(" [NO-DATA refuse]");
#endif
            xcb_selection_notify_event_t sn = { 0 };
            sn.response_type = XCB_SELECTION_NOTIFY;
            sn.requestor = ev->requestor;
            sn.selection = ev->selection;
            sn.target   = ev->target;
            sn.property = XCB_NONE;
            sn.time     = ev->time;
            xcb_send_event(conn, 0, ev->requestor, 0, (const char *)&sn);
            xcb_flush(conn);
            return 0;
        }

        /* Serve the image data, using INCR for large payloads */
        uint32_t maxreq = xcb_get_maximum_request_length(conn) * 4 - 24;
        if (maxreq > 262144) maxreq = 262144;

        if (g_sel_len > maxreq) {
            send_incr(conn, ev->requestor, ev->property, ev->selection,
                      ev->target, g_sel_data, g_sel_len);
        } else {
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, ev->requestor,
                                ev->property, ev->target, 8,
                                (uint32_t)g_sel_len, g_sel_data);

            xcb_selection_notify_event_t sn = { 0 };
            sn.response_type = XCB_SELECTION_NOTIFY;
            sn.requestor = ev->requestor;
            sn.selection = ev->selection;
            sn.target   = ev->target;
            sn.property = ev->property;
            sn.time     = ev->time;
            xcb_send_event(conn, 0, ev->requestor, 0, (const char *)&sn);
            xcb_flush(conn);
        }
        g_sel_delivered = 1;
        return 1;
    }

    /* Unknown target — refuse */
#ifdef VERBOSE
    log_msg(" [UNK tgt=%s prop=%s]", dbg_atom_name(conn, ev->target),
            dbg_atom_name(conn, ev->property));
#endif
    xcb_selection_notify_event_t sn = { 0 };
    sn.response_type = XCB_SELECTION_NOTIFY;
    sn.requestor = ev->requestor;
    sn.selection = ev->selection;
    sn.target   = ev->target;
    sn.property = XCB_NONE;
    sn.time     = ev->time;
    xcb_send_event(conn, 0, ev->requestor, 0, (const char *)&sn);
    xcb_flush(conn);
    return 0;
}

/*
 * Process all pending X11 events.  Returns:
 *   1 if a SelectionRequest was handled (TARGETS probe or image request)
 *   0 if no relevant event
 *  -1 on error
 */
static int process_x11(xcb_connection_t *conn)
{
    int had_request = 0;
    int event_count = 0;
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(conn))) {
        uint8_t rt = ev->response_type & ~0x80;
        event_count++;
        if (rt == XCB_SELECTION_REQUEST) {
            had_request = handle_selection_request(conn,
                           (xcb_selection_request_event_t *)ev) || had_request;
        } else if (rt == XCB_SELECTION_NOTIFY) {
            xcb_selection_notify_event_t *sn = (xcb_selection_notify_event_t *)ev;
#ifdef VERBOSE
            log_msg(" [SN: req=0x%x tgt=%s prop=%s]",
                    (unsigned)sn->requestor,
                    dbg_atom_name(conn, sn->target),
                    dbg_atom_name(conn, sn->property));
#endif
            if (sn->property == XCB_NONE) {
                g_recv_done = -2; /* conversion refused */
#ifdef VERBOSE
                log_msg(" [SN_REFUSE tgt=0x%x]", sn->target);
#endif
            } else if (sn->target == xa.targets) {
                /* Read TARGETS list from property */
                xcb_get_property_cookie_t ck =
                    xcb_get_property(conn, 1, sn->requestor, sn->property,
                                     XCB_ATOM_ANY, 0, 65536);
                xcb_get_property_reply_t *rp =
                    xcb_get_property_reply(conn, ck, NULL);
                if (rp) {
                    int n = xcb_get_property_value_length(rp) / 4;
                    xcb_atom_t *atms = (xcb_atom_t *)xcb_get_property_value(rp);
#ifdef VERBOSE
                    log_msg(" [TGT:");
                    for (int k = 0; k < n; k++)
                        log_msg(" %s", dbg_atom_name(conn, atms[k]));
                    log_msg("]");
#endif
                    /* Stash the first image target we find in g_recv_done */
                    for (int k = 0; k < n; k++) {
                        if (atom_to_format(atms[k])) {
                            g_sel_fmt = atms[k];
                            g_recv_done = 1;
                            break;
                        }
                    }
                    if (g_recv_done != 1) {
#ifdef VERBOSE
                        log_msg(" [TGT_noimg n=%d]", n);
#endif
                        g_recv_done = -2;
                    }
                    free(rp);
                } else {
#ifdef VERBOSE
                    log_msg(" [TGT_noprop]");
#endif
                    g_recv_done = -2;
                }
            } else {
                /* Image data: read property */
#ifdef VERBOSE
                log_msg(" [IMG-SN: reading prop=%s from req=0x%x]",
                        dbg_atom_name(conn, sn->property),
                        (unsigned)sn->requestor);
#endif
                xcb_get_property_cookie_t ck =
                    xcb_get_property(conn, 1, sn->requestor, sn->property,
                                     XCB_ATOM_ANY, 0, 262144);
                xcb_get_property_reply_t *rp =
                    xcb_get_property_reply(conn, ck, NULL);
                if (rp) {
#ifdef VERBOSE
                    log_msg(" [IMG-PROP: type=%s fmt=%u valuelen=%u "
                            "bytes_after=%u]",
                            dbg_atom_name(conn, rp->type),
                            (unsigned)rp->format,
                            (unsigned)rp->value_len,
                            (unsigned)rp->bytes_after);
#endif
                    if (rp->type == xa.incr) {
                        /* INCR start: allocate accumulation buffer */
                        uint32_t lb = *(uint32_t *)
                            xcb_get_property_value(rp);
#ifdef VERBOSE
                        log_msg(" [INCR-START lower=%u]", (unsigned)lb);
#endif
                        free(g_recv_incr);
                        g_recv_incr = (unsigned char *)malloc(lb);
                        g_recv_incr_size = 0;
                    } else {
                        int n = xcb_get_property_value_length(rp);
                        free(g_recv_incr);
                        g_recv_incr = (unsigned char *)malloc((size_t)n);
                        if (g_recv_incr) {
                            memcpy(g_recv_incr,
                                   xcb_get_property_value(rp), (size_t)n);
                            g_recv_incr_size = (size_t)n;
                        }
                        g_recv_done = 1;
#ifdef VERBOSE
                        log_msg(" [IMG-RECV: %d bytes, done=%d]", n, g_recv_done);
#endif
                    }
                    free(rp);
                } else {
#ifdef VERBOSE
                    log_msg(" [IMG-NOPROP: xcb_get_property returned NULL]");
#endif
                }
            }
        } else if (rt == XCB_PROPERTY_NOTIFY) {
            xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
#ifdef VERBOSE
            log_msg(" [PN: atom=%s win=0x%x state=%u]",
                    dbg_atom_name(conn, pn->atom),
                    (unsigned)pn->window, (unsigned)pn->state);
#endif
            if (pn->state == XCB_PROPERTY_NEW_VALUE && g_recv_incr) {
                xcb_get_property_cookie_t ck =
                    xcb_get_property(conn, 1, pn->window, pn->atom,
                                     XCB_ATOM_ANY, 0, 262144);
                xcb_get_property_reply_t *rp =
                    xcb_get_property_reply(conn, ck, NULL);
                if (rp) {
                    int n = xcb_get_property_value_length(rp);
#ifdef VERBOSE
                    log_msg(" [INCR-chunk: %d bytes]", n);
#else
                    log_msg(".");
#endif
                    if (n == 0) {
                        g_recv_done = 1; /* INCR complete */
#ifdef VERBOSE
                        log_msg(" [INCR-DONE zero-length terminator]");
#endif
                    } else {
                        g_recv_incr = (unsigned char *)realloc(g_recv_incr,
                            g_recv_incr_size + (size_t)n);
                        if (g_recv_incr) {
                            memcpy(g_recv_incr + g_recv_incr_size,
                                   xcb_get_property_value(rp), (size_t)n);
                            g_recv_incr_size += (size_t)n;
                        }
                    }
                    free(rp);
                } else {
#ifdef VERBOSE
                    log_msg(" [INCR-NOPROP: xcb_get_property returned NULL]");
#endif
                }
            }
        } else if (rt == XCB_SELECTION_CLEAR) {
            xcb_selection_clear_event_t *sc = (xcb_selection_clear_event_t *)ev;
#ifdef VERBOSE
            log_msg(" [SELECTION_CLEAR sel=%s]", dbg_atom_name(conn, sc->selection));
#endif
            if (sc->selection == xa.clipboard)
                g_sel_lost = 1;
        } else {
#ifdef VERBOSE
            log_msg(" [EV:%u]", (unsigned)rt);
#endif
        }
        free(ev);
    }
#ifdef VERBOSE
    if (event_count > 0 && !had_request)
        log_msg(" [X11-ev:%d noSR]", event_count);
#endif
    return had_request;
}

/* Wait for events on both X11 fd and socket fd, up to timeout_ms.
   Returns: 1=X11 event, 2=socket ready, 0=timeout, -1=error */
static int wait_events(int x11_fd, int sock_fd, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(x11_fd, &rfds);
    FD_SET(sock_fd, &rfds);
    int nfds = (x11_fd > sock_fd ? x11_fd : sock_fd) + 1;

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(nfds, &rfds, NULL, NULL, &tv);
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    if (FD_ISSET(x11_fd, &rfds)) return 1;
    if (FD_ISSET(sock_fd, &rfds)) return 2;
    return 0;
}

/* ---------- file path helpers ---------- */

static void build_input_path_fmt(const struct cliptest *ct, uint32_t fmt,
                                  char *path, size_t sz)
{
    const char *s = size_name(ct->width);
    const char *mc = (ct->transparency == CLIPTEST_TRANSPARENCY_YES)
                     ? "circle" : "square";
    const char *ext = format_ext(fmt);
    snprintf(path, sz, INPUT_DIR "/dalmatian-%s-%s.%s", s, mc, ext);
}

static void build_output_path(int index, const struct cliptest *ct,
                               const char *ext, char *path, size_t sz)
{
    const char *s = size_name(ct->width);
    const char *mc = (ct->transparency == CLIPTEST_TRANSPARENCY_YES)
                     ? "circle" : "square";
    snprintf(path, sz, OUTPUT_DIR "/%04d-%s-%s.%s", index, s, mc, ext);
}

/* ---------- test execution ---------- */

static int run_tests(int sock_fd, xcb_connection_t *conn, xcb_window_t win,
                     struct cliptest *tests, int num_tests,
                     int start_idx, int end_idx, FILE *results_fp)
{
    int passed = 0, failed = 0;
    int x11_fd = xcb_get_file_descriptor(conn);

    mkdir(OUTPUT_DIR, 0755);

    /* Enable debug logging in VcXsrv by setting CLIPTEST_DBG_ON on its
       iWindow.  Find the window by querying the CLIPBOARD owner. */
    {
        xcb_get_selection_owner_cookie_t ck =
            xcb_get_selection_owner(conn, xa.clipboard);
        xcb_get_selection_owner_reply_t *rp =
            xcb_get_selection_owner_reply(conn, ck, NULL);
        if (rp && rp->owner != XCB_NONE) {
            g_vcxsrv_win = rp->owner;
#ifdef VERBOSE
            log_msg(" [VcXsrv window=0x%x]", (unsigned)g_vcxsrv_win);
#endif
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, g_vcxsrv_win,
                                xa.debug_on, XCB_ATOM_STRING, 8, 4, "GO");
            xcb_flush(conn);
#ifdef VERBOSE
            log_msg(" [DEBUG LOG ON]");
#endif
            usleep(100000);
        }
        free(rp);
    }

    for (int i = start_idx; i <= end_idx && i < num_tests; i++) {
        struct cliptest *ct = &tests[i];

        log_msg("[%04d] %s %s->%s %s hdrop=%s transp=%s",
                i, direction_name(ct->direction),
                format_name(ct->from_format), format_name(ct->to_format),
                size_name(ct->width), hdrop_name(ct->hdrop),
                transparency_name(ct->transparency));

        time_t t_start = time(NULL);
        unsigned char *resp_image = NULL;
        size_t resp_image_len = 0;
        char resp_ext[8] = {0};
        uint32_t success = 0;

        if (ct->direction == CLIPTEST_X11_TO_WIN32) {
            /* ----- Linux is Copy source ----- */

            /* 1. Load the input image */
            char input_path[512];
            build_input_path_fmt(ct, ct->from_format, input_path,
                                 sizeof(input_path));
            FILE *fp = fopen(input_path, "rb");
            if (!fp) {
                log_msg(" - SKIP: cannot open %s\n", input_path);
                fprintf(results_fp, "%04d SKIP cannot_open %s\n", i, input_path);
                failed++;
                continue;
            }
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz <= 0 || (size_t)sz > MAX_IMAGE_SIZE) {
                log_msg(" - SKIP: bad file size\n");
                fclose(fp);
                failed++;
                continue;
            }
            g_sel_len = (size_t)sz;
            g_sel_data = (unsigned char *)malloc(g_sel_len);
            if (!g_sel_data || fread(g_sel_data, 1, g_sel_len, fp) != g_sel_len) {
                log_msg(" - SKIP: read failed\n");
                free(g_sel_data); g_sel_data = NULL;
                fclose(fp);
                failed++;
                continue;
            }
            fclose(fp);
            g_sel_fmt = ct->from_format;
            g_sel_delivered = 0;

            /* 2. Take CLIPBOARD ownership */
            xcb_set_selection_owner(conn, win, xa.clipboard, XCB_CURRENT_TIME);
            xcb_flush(conn);

            /* 3. Wait for VcXsrv's TARGETS probe */
#ifdef VERBOSE
            log_msg(" [waiting for TARGETS probe]");
#endif
            int probe_done = 0;
            while (!probe_done && !timed_out(t_start)) {
                int ev = wait_events(x11_fd, sock_fd, 1000);
                if (ev == 1) {
                    if (process_x11(conn))
                        probe_done = 1;
                }
            }
            if (!probe_done) {
                if (timed_out(t_start)) log_msg(" [TIMEOUT]");
                log_msg(" - timeout waiting for TARGETS probe\n");
                fprintf(results_fp, "%04d FAIL no_targets_probe\n", i);
                free(g_sel_data); g_sel_data = NULL;
                failed++;
                continue;
            }

            /* Give VcXsrv time to process the TARGETS reply and add
               CF_DIB/CF_DIBV5 to the Windows clipboard via
               handleSelectionNotify before win32cliptest calls
               GetClipboardData.  Without this delay GetClipboardData
               returns NULL and WM_RENDERFORMAT goes to fake_paste. */
            usleep(300000);
#ifdef VERBOSE
            log_msg(" [CF_DIB wait done]");
#endif

            /* 4. Send request to Windows */
            uint32_t req_hdr[3] = { MSG_REQUEST,
                CLIPTEST_REQUEST_BASE, (uint32_t)i };
            if (send_all(sock_fd, req_hdr, 12) < 0 ||
                send_all(sock_fd, ct, CLIPTEST_STRUCT_SIZE) < 0) {
                log_msg(" - send failed\n");
                free(g_sel_data); g_sel_data = NULL;
                failed++;
                break;
            }
#ifdef VERBOSE
            log_msg(" [sent paste cmd]");
#endif

            /* 5. Wait for socket response from win32cliptest.
               win32cliptest calls GetClipboardData which triggers
               VcXsrv's WM_RENDERFORMAT.  VcXsrv handles the X11
               selection serving internally, so we only need to wait
               for the socket response that carries the result image. */
            int sock_done = 0;
            g_sel_delivered = 0;
            while (!sock_done && !timed_out(t_start)) {
                int ev = wait_events(x11_fd, sock_fd, 1000);
                if (ev == 1) {
                    process_x11(conn);
                } else if (ev == 2) {
                    /* Read socket response */
                    uint32_t rh[4];
                    if (recv_all(sock_fd, rh, 16) == 0 && rh[0] == MSG_RESPONSE) {
                        success = rh[3];
                        uint32_t rlen = rh[1];
                        if (recv_all(sock_fd, resp_ext, 8) < 0)
                            memset(resp_ext, 0, 8);
                        if (rlen > CLIPTEST_RESPONSE_BASE) {
                            resp_image_len = rlen - CLIPTEST_RESPONSE_BASE;
                            resp_image = (unsigned char *)malloc(resp_image_len);
                            if (!resp_image ||
                                recv_all(sock_fd, resp_image, resp_image_len) < 0) {
                                free(resp_image);
                                resp_image = NULL;
                                resp_image_len = 0;
                            }
                        }
                        sock_done = 1;
                    }
                }
            }
            free(g_sel_data);
            g_sel_data = NULL;
            g_sel_fmt = 0;

            /* Release CLIPBOARD ownership so VcXsrv can re-assert it
               before the next test.  Without this, CLIPBOARD stays
               owned by our window even though we freed g_sel_data,
               causing subsequent WIN32_TO_X11 tests to fail. */
            {
                xcb_set_selection_owner(conn, XCB_NONE, xa.clipboard,
                                        XCB_CURRENT_TIME);
                xcb_flush(conn);
#ifdef VERBOSE
                log_msg(" [CLIPBOARD released]");
#endif
            }

            if (!sock_done) {
                log_msg(" - timeout waiting for socket response (timed_out=%d)\n",
                        (int)timed_out(t_start));
                failed++;
                continue;
            }
        } else {
            /* ----- Win32 is Copy source (WIN32_TO_X11) -----
             * Do NOT take CLIPBOARD ownership first — VcXsrv already owns it
             * from its normal monitoring.  Just tell win32cliptest to populate
             * the Windows clipboard, then request the image from VcXsrv via
             * xcb_convert_selection, exactly as xsimpleclip -paste does. */

            /* 1. Send request to Windows */
            uint32_t req_hdr[3] = { MSG_REQUEST,
                CLIPTEST_REQUEST_BASE, (uint32_t)i };
            if (send_all(sock_fd, req_hdr, 12) < 0 ||
                send_all(sock_fd, ct, CLIPTEST_STRUCT_SIZE) < 0) {
                log_msg(" - send failed\n");
                failed++;
                break;
            }
#ifdef VERBOSE
            log_msg(" [sent copy cmd]");
#endif

            /* 2. Wait for ack from win32cliptest.
               Must drain X11 events so they don't starve the socket read. */
            uint32_t rh[4];
            int ack_done = 0;
            while (!ack_done && !timed_out(t_start)) {
                int ev = wait_events(x11_fd, sock_fd, 1000);
                if (ev == 2) {
                    if (recv_all(sock_fd, rh, 16) == 0 &&
                        rh[0] == MSG_RESPONSE) {
                        success = rh[3];
                        if (recv_all(sock_fd, resp_ext, 8) < 0)
                            memset(resp_ext, 0, 8);
                        ack_done = 1;
#ifdef VERBOSE
                        log_msg(" [ack]");
#endif
                    }
                } else if (ev == 1) {
                    process_x11(conn);
                }
            }
            if (!ack_done) {
                if (timed_out(t_start)) log_msg(" [TIMEOUT]");
                log_msg(" - timeout waiting for ack\n");
                failed++;
                continue;
            }
            if (!success) {
                log_msg(" - win32cliptest failed\n");
                fprintf(results_fp, "%04d FAIL win32_failed\n", i);
                failed++;
                continue;
            }

            usleep(200000);

            /* 3. Wait for VcXsrv to process WM_CLIPBOARDUPDATE and
               re-assert CLIPBOARD.  Poll until VcXsrv owns it. */
            {
                int owner_ok = 0;
                int owner_polls = 0;
                while (!owner_ok && !timed_out(t_start) && owner_polls < 40) {
                    xcb_get_selection_owner_cookie_t ck =
                        xcb_get_selection_owner(conn, xa.clipboard);
                    xcb_get_selection_owner_reply_t *rp =
                        xcb_get_selection_owner_reply(conn, ck, NULL);
                    if (rp) {
                        if (rp->owner != XCB_NONE && rp->owner != win) {
                            owner_ok = 1;
                            g_vcxsrv_win = rp->owner;
#ifdef VERBOSE
                            log_msg(" [owner=0x%x]", (unsigned)rp->owner);
#endif
                        }
                        free(rp);
                    }
                    usleep(200000);
                    if (!owner_ok) {
                        owner_polls++;
                    }
                }
                if (!owner_ok) {
                    log_msg(" - CLIPBOARD unowned (owner_polls=%d)\n",
                            owner_polls);
                    fprintf(results_fp, "%04d FAIL clipboard_unowned\n", i);
                    failed++;
                    continue;
                }
            }

            /* 4. Request TARGETS first to see what VcXsrv advertises,
               then request the matching image format. */
            g_recv_done = 0;
            free(g_recv_incr);
            g_recv_incr = NULL;
            g_recv_incr_size = 0;

#ifdef VERBOSE
            log_msg(" [xcb_convert_selection TARGETS req=0x%x prop=%s]",
                    (unsigned)win, dbg_atom_name(conn, xa.property));
#endif
            xcb_convert_selection(conn, win, xa.clipboard, xa.targets,
                                  xa.property, XCB_CURRENT_TIME);
            xcb_flush(conn);

            while (!g_recv_done && !timed_out(t_start)) {
                int ev = wait_events(x11_fd, sock_fd, 1000);
                if (ev == 1) process_x11(conn);
            }

            if (g_recv_done != 1) {
                log_msg(" - TARGETS failed (g_recv_done=%d)\n", g_recv_done);
                fprintf(results_fp, "%04d FAIL targets_failed\n", i);
                failed++;
                continue;
            }

            /* 5. Now request the actual image format.
               Use the to_format's corresponding X11 atom. */
            {
                xcb_atom_t img_target = format_to_atom(ct->to_format);
                if (!img_target) {
                    log_msg(" - no X11 atom for format %s\n",
                            format_name(ct->to_format));
                    fprintf(results_fp, "%04d FAIL no_atom_for_format\n", i);
                    failed++;
                    continue;
                }

                g_recv_done = 0;
                free(g_recv_incr);
                g_recv_incr = NULL;
                g_recv_incr_size = 0;

#ifdef VERBOSE
                log_msg(" [xcb_convert_selection IMG tgt=%s req=0x%x prop=%s]",
                        dbg_atom_name(conn, img_target),
                        (unsigned)win, dbg_atom_name(conn, xa.property));
#endif
                xcb_convert_selection(conn, win, xa.clipboard, img_target,
                                      xa.property, XCB_CURRENT_TIME);
                xcb_flush(conn);

                while (!g_recv_done && !timed_out(t_start)) {
                    int ev = wait_events(x11_fd, sock_fd, 1000);
                    if (ev == 1) process_x11(conn);
                }

                if (g_recv_done != 1) {
                    log_msg(" - image receive failed (g_recv_done=%d)\n",
                            g_recv_done);
                    fprintf(results_fp, "%04d FAIL image_receive_failed\n", i);
                    failed++;
                    continue;
                }
            }

            if (g_recv_incr && g_recv_incr_size > 0) {
                resp_image = g_recv_incr;
                resp_image_len = g_recv_incr_size;
                g_recv_incr = NULL;
                g_recv_incr_size = 0;
                success = 1;
            } else {
                log_msg(" - no image data received\n");
                fprintf(results_fp, "%04d FAIL no_image_data\n", i);
                failed++;
                continue;
            }
        }

        /* Write result image */
        char output_path[512];
        build_output_path(i, ct,
                          resp_ext[0] ? resp_ext : format_ext(ct->to_format),
                          output_path, sizeof(output_path));

        int wrote_file = 0;
        if (resp_image && resp_image_len > 0) {
            FILE *ofp = fopen(output_path, "wb");
            if (ofp) {
                fwrite(resp_image, 1, resp_image_len, ofp);
                fclose(ofp);
                wrote_file = 1;
            }
        }

        /* --- analysis (same logic as before) --- */
        int md5_ok = -1;
        int transp_ok = 0, transp_count = -1;
        const char *transp_gray_msg = "transp";
        int has_transparency = (ct->transparency == CLIPTEST_TRANSPARENCY_YES) ? 1 : 0;
        int transp_expected = expected_transparent(ct->width, has_transparency);

        if (wrote_file && ct->from_format == ct->to_format
            && (ct->direction == CLIPTEST_X11_TO_WIN32
                || ct->hdrop == CLIPTEST_HDROP_YES)) {
            /* md5sum meaningful when no transcoding happens:
               X11_TO_WIN32 can pass through, and WIN32_TO_X11 with
               hdrop=yes uses convertFnRawToRaw (no DIB re-encode). */
            char input_md5[128] = "", output_md5[128] = "";
            char input_path_fmt[512];
            build_input_path_fmt(ct, ct->from_format, input_path_fmt,
                                 sizeof(input_path_fmt));
            if (md5sum_file(input_path_fmt, input_md5, sizeof(input_md5)) == 0 &&
                md5sum_file(output_path, output_md5, sizeof(output_md5)) == 0) {
                md5_ok = (strcmp(input_md5, output_md5) == 0) ? 1 : 0;
            }
        }

        if (md5_ok < 0 && wrote_file) {
            /* X11_TO_WIN32 always produces raw DIB/DIBV5 (indicated by
               resp_ext).  WIN32_TO_X11 may produce DIB/DIBV5 if that was
               the to_format. */
            int is_dib_output = (resp_ext[0] != 0) ||
                (ct->to_format == CLIPTEST_FORMAT_DIB) ||
                (ct->to_format == CLIPTEST_FORMAT_DIBV5);
            if (is_dib_output) {
                FILE *fp = fopen(output_path, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    unsigned char *dib_data = (unsigned char *)malloc((size_t)sz);
                    if (dib_data && fread(dib_data, 1, (size_t)sz, fp) == (size_t)sz) {
                        int dw, dh;
                        unsigned char *rgba = dibimg_load(dib_data, (size_t)sz, &dw, &dh);
                        if (rgba) {
                            uint16_t dib_bits = *(uint16_t *)(dib_data + 14);
                            int has_dib_alpha = (dib_bits == 32);
                            if (has_dib_alpha) {
                                transp_count = 0;
                                int total = dw * dh;
                                for (int p = 0; p < total; p++)
                                    if (rgba[p * 4 + 3] < 128) transp_count++;
                                int src_has_alpha_dib =
                                    (ct->from_format == CLIPTEST_FORMAT_PNG ||
                                     ct->from_format == CLIPTEST_FORMAT_GIF ||
                                     ct->from_format == CLIPTEST_FORMAT_DIBV5);
                                if (!src_has_alpha_dib) transp_expected = 0;
                            } else {
                                transp_count = 0;
                                int total = dw * dh;
                                for (int p = 0; p < total; p++) {
                                    int r = rgba[p * 4 + 0];
                                    int g = rgba[p * 4 + 1];
                                    int b = rgba[p * 4 + 2];
                                    if (abs(r - 128) <= 10 && abs(g - 128) <= 10 &&
                                        abs(b - 128) <= 10) transp_count++;
                                }
                                transp_gray_msg = "graybg";
                            }
                            transp_ok = within_tolerance(transp_count, transp_expected, dw);
                            free(rgba);
                        }
                    }
                    free(dib_data);
                    fclose(fp);
                }
            } else {
                int src_has_alpha = (ct->from_format == CLIPTEST_FORMAT_PNG ||
                                     ct->from_format == CLIPTEST_FORMAT_GIF ||
                                     ct->from_format == CLIPTEST_FORMAT_DIBV5 ||
                                     (ct->direction == CLIPTEST_WIN32_TO_X11 &&
                                      ct->transparency == CLIPTEST_TRANSPARENCY_YES &&
                                      !(ct->hdrop == CLIPTEST_HDROP_YES && 
                                                (ct->from_format == CLIPTEST_FORMAT_BMP || ct->from_format == CLIPTEST_FORMAT_JPEG))));
                int dst_has_alpha = (ct->to_format == CLIPTEST_FORMAT_PNG ||
                                     ct->to_format == CLIPTEST_FORMAT_GIF);
                int dst_might_have_alpha = (ct->to_format == CLIPTEST_FORMAT_BMP);
                if (src_has_alpha && dst_might_have_alpha) {
/* we make a special case for BMP to allow both transparency or flatness
 * in the output since BMP is somewhat of an ambiguous case and we don't
 * really want to process it when it is can be passed through "unmodifed" from DIB: */
                    int transp_coun_;
                    transp_count = count_transparent_pixels(output_path);
                    transp_coun_ = count_gray_pixels(output_path);
                    if (transp_count < transp_coun_)
                        transp_count = transp_coun_;
                    transp_ok = within_tolerance(transp_count, transp_expected, ct->width);
                    transp_gray_msg = "transpORgray";
                } else if (src_has_alpha && dst_has_alpha) {
                    transp_count = count_transparent_pixels(output_path);
                    transp_ok = within_tolerance(transp_count, transp_expected, ct->width);
                    transp_gray_msg = "transp";
                } else {
                    transp_count = count_gray_pixels(output_path);
                    transp_ok = within_tolerance(transp_count, transp_expected, ct->width);
                    transp_gray_msg = "graybg";
                }
            }
        }

        if (md5_ok >= 0) {
            log_msg(" - success=%" PRIu32 " wrote=%d md5sum=%s%s\n",
                    success, wrote_file,
                    md5_ok ? "match" : "NO-MATCH",
                    md5_ok ? " OK" : " FAIL");
        } else {
            log_msg(" - success=%" PRIu32 " wrote=%d %s=%d(expected %d)%s\n",
                    success, wrote_file,
                    transp_gray_msg,
                    transp_count, transp_expected,
                    transp_ok ? " OK" : " FAIL");
        }

        if (md5_ok >= 0) {
            fprintf(results_fp, "%04d %s success=%" PRIu32 " wrote=%d "
                    "md5sum=%s fmt=%s->%s size=%s\n",
                    i, direction_name(ct->direction), success, wrote_file,
                    md5_ok ? "match" : "NO-MATCH",
                    format_name(ct->from_format), format_name(ct->to_format),
                    size_name(ct->width));
            if (md5_ok) passed++; else failed++;
        } else {
            fprintf(results_fp, "%04d %s success=%" PRIu32 " wrote=%d "
                    "%s_actual=%d %s_expected=%d %s_ok=%d "
                    "fmt=%s->%s size=%s\n",
                    i, direction_name(ct->direction), success, wrote_file,
                    transp_gray_msg, transp_count,
                    transp_gray_msg, transp_expected,
                    transp_gray_msg, transp_ok,
                    format_name(ct->from_format), format_name(ct->to_format),
                    size_name(ct->width));
            if (transp_ok) passed++; else failed++;
        }
        free(resp_image);
    }

    /* Disable debug logging in VcXsrv */
    if (g_vcxsrv_win != XCB_NONE) {
        xcb_delete_property(conn, g_vcxsrv_win, xa.debug_on);
        xcb_flush(conn);
#ifdef VERBOSE
        log_msg(" [DEBUG LOG OFF]");
#endif
    }

    log_msg("\nResults: %d passed, %d failed\n", passed, failed);
    fprintf(results_fp, "\nTotal: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

/* ---------- usage ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -host <addr> [-start N] [-end N] [-test N]\n"
            "  -host  <addr>   IPv4 address of win32cliptest server\n"
            "  -start N        First test index to run (default: 0)\n"
            "  -end   N        Last test index to run (default: all)\n"
            "  -test  N        Run a single test index\n"
            "  -list           List all test indices (no execution)\n"
            "  -quit           Send quit message to shut down the server\n"
            "  -display NAME   X server to connect to (default: $DISPLAY)\n"
            "  -stop-x-server  Set CLIPTEST_DBG_OFF to close debug log and exit X server\n",
            prog);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    const char *host = NULL;
    const char *display_name = NULL;
    int start_idx = 0;
    int end_idx   = -1;
    int single_test = -1;
    int list_only   = 0;
    int quit_mode   = 0;
    int stop_x_server = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-host") == 0 && i + 1 < argc)
            host = argv[++i];
        else if (strcmp(argv[i], "-start") == 0 && i + 1 < argc)
            start_idx = atoi(argv[++i]);
        else if (strcmp(argv[i], "-end") == 0 && i + 1 < argc)
            end_idx = atoi(argv[++i]);
        else if (strcmp(argv[i], "-test") == 0 && i + 1 < argc) {
            single_test = atoi(argv[++i]);
            start_idx = single_test;
            end_idx   = single_test;
        } else if (strcmp(argv[i], "-list") == 0)
            list_only = 1;
        else if (strcmp(argv[i], "-quit") == 0)
            quit_mode = 1;
        else if (strcmp(argv[i], "-stop-x-server") == 0)
            stop_x_server = 1;
        else if (strcmp(argv[i], "-display") == 0 && i + 1 < argc)
            display_name = argv[++i];
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!host) {
        usage(argv[0]);
        return 1;
    }

    if (stop_x_server) {
        /* -stop-x-server: find VcXsrv's iWindow and set CLIPTEST_DBG_OFF.
           Try CLIPBOARD owner, then PRIMARY owner, then walk the window
           tree searching for a window titled "xwinclip". */
        if (create_running_window(display_name) < 0) {
            fprintf(stderr, "Failed to connect to X server\n");
            return 1;
        }
        intern_all_atoms(g_x11_conn);

        xcb_window_t target = XCB_NONE;

        /* Strategy 1: CLIPBOARD owner */
        {
            xcb_get_selection_owner_cookie_t ck =
                xcb_get_selection_owner(g_x11_conn, xa.clipboard);
            xcb_get_selection_owner_reply_t *rp =
                xcb_get_selection_owner_reply(g_x11_conn, ck, NULL);
            if (rp && rp->owner != XCB_NONE) {
                target = rp->owner;
                log_msg("Found VcXsrv via CLIPBOARD: 0x%x\n", (unsigned)target);
            }
            free(rp);
        }

        /* Strategy 2: PRIMARY owner */
        if (target == XCB_NONE) {
            xcb_get_selection_owner_cookie_t ck =
                xcb_get_selection_owner(g_x11_conn, XCB_ATOM_PRIMARY);
            xcb_get_selection_owner_reply_t *rp =
                xcb_get_selection_owner_reply(g_x11_conn, ck, NULL);
            if (rp && rp->owner != XCB_NONE) {
                target = rp->owner;
                log_msg("Found VcXsrv via PRIMARY: 0x%x\n", (unsigned)target);
            }
            free(rp);
        }

        /* Strategy 3: walk window tree for "xwinclip" */
        if (target == XCB_NONE && xa.wm_name != XCB_ATOM_NONE) {
            log_msg("CLIPBOARD and PRIMARY both unowned, "
                    "walking window tree for xwinclip...\n");
            xcb_screen_t *screen = xcb_setup_roots_iterator(
                xcb_get_setup(g_x11_conn)).data;
            xcb_window_t root = screen->root;

            /* BFS over the window tree.  Keep a simple fixed-size queue;
               VcXsrv is a single-window app so we won't go deep. */
            xcb_window_t queue[4096];
            int qhead = 0, qtail = 0;
            queue[qtail++] = root;
            int checked = 0;

            while (qhead < qtail && target == XCB_NONE) {
                xcb_window_t w = queue[qhead++];

                xcb_query_tree_cookie_t qtc =
                    xcb_query_tree(g_x11_conn, w);
                xcb_query_tree_reply_t *qt =
                    xcb_query_tree_reply(g_x11_conn, qtc, NULL);
                if (!qt) continue;

                int nc = xcb_query_tree_children_length(qt);
                xcb_window_t *children = xcb_query_tree_children(qt);

                for (int i = 0; i < nc && target == XCB_NONE; i++) {
                    xcb_window_t cw = children[i];

                    /* Check WM_NAME */
                    xcb_get_property_cookie_t pc =
                        xcb_get_property(g_x11_conn, 0, cw,
                                         xa.wm_name,
                                         XCB_ATOM_STRING, 0, 32);
                    xcb_get_property_reply_t *pr =
                        xcb_get_property_reply(g_x11_conn, pc, NULL);
                    checked++;
                    if (pr) {
                        int nlen = xcb_get_property_value_length(pr);
                        const char *name = xcb_get_property_value(pr);
                        if (nlen == 8 && memcmp(name, "xwinclip", 8) == 0) {
                            target = cw;
                            log_msg("Found VcXsrv via tree walk: 0x%x "
                                    "(checked %d windows)\n",
                                    (unsigned)target, checked);
                        }
                        free(pr);
                    }

                    /* Enqueue children for later */
                    if (qtail < 4096)
                        queue[qtail++] = cw;
                }
                free(qt);
            }

            if (target == XCB_NONE) {
                log_msg("Tree walk checked %d windows, "
                        "none matched xwinclip\n", checked);
            }
        }

        if (target != XCB_NONE) {
            log_msg("Setting CLIPTEST_DBG_OFF on window 0x%x\n",
                    (unsigned)target);
            xcb_change_property(g_x11_conn, XCB_PROP_MODE_REPLACE, target,
                                xa.debug_off, XCB_ATOM_STRING, 8, 4, "END");
            xcb_flush(g_x11_conn);
            log_msg("X server shutdown triggered.\n");
        } else {
            fprintf(stderr, "Could not find VcXsrv window "
                    "(no CLIPBOARD/PRIMARY owner, no xwinclip window)\n");
        }
        destroy_running_window();
        return (target != XCB_NONE) ? 0 : 1;
    }

    /* Generate test array */
    struct cliptest *tests = NULL;
    int num_tests = generate_tests(&tests);
    if (!num_tests) {
        fprintf(stderr, "Failed to generate test array\n");
        return 1;
    }

    if (end_idx < 0 || end_idx >= num_tests)
        end_idx = num_tests - 1;

    log_msg("Test array: %d tests total, running [%d, %d]\n",
            num_tests, start_idx, end_idx);

    if (list_only) {
        for (int i = start_idx; i <= end_idx && i < num_tests; i++) {
            struct cliptest *ct = &tests[i];
            printf("[%04d] %s %s->%s %s hdrop=%s transp=%s\n",
                   i, direction_name(ct->direction),
                   format_name(ct->from_format), format_name(ct->to_format),
                   size_name(ct->width), hdrop_name(ct->hdrop),
                   transparency_name(ct->transparency));
        }
        free(tests);
        return 0;
    }

    /* Connect to Windows server */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        free(tests);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "Cannot resolve %s\n", host);
            close(sock_fd);
            free(tests);
            return 1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    log_msg("Connecting to %s:%d ...\n", host, DEFAULT_PORT);
    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock_fd);
        free(tests);
        return 1;
    }
    log_msg("Connected.\n");

    if (quit_mode) {
        uint32_t quit_hdr[3] = { MSG_QUIT, 12, 0 };
        log_msg("Sending MSG_QUIT...\n");
        send_all(sock_fd, quit_hdr, 12);
        close(sock_fd);
        free(tests);
        log_msg("Done.\n");
        return 0;
    }

    if (create_running_window(display_name) < 0) {
        fprintf(stderr, "Failed to connect to X server\n");
        close(sock_fd);
        free(tests);
        return 1;
    }

    intern_all_atoms(g_x11_conn);
    log_msg("X11 atoms interned: CLIPBOARD=0x%x TARGETS=0x%x image/png=0x%x image/bmp=0x%x image/gif=0x%x image/jpeg=0x%x property=0x%x\n",
            (unsigned)xa.clipboard, (unsigned)xa.targets,
            (unsigned)xa.png, (unsigned)xa.bmp, (unsigned)xa.gif, (unsigned)xa.jpeg,
            (unsigned)xa.property);

    /* Open results file */
    FILE *results_fp = fopen(RESULTS_FILE, "w");
    if (!results_fp) {
        perror(RESULTS_FILE);
        close(sock_fd);
        free(tests);
        destroy_running_window();
        return 1;
    }

    int rc = run_tests(sock_fd, g_x11_conn, g_x11_win, tests, num_tests,
                      start_idx, end_idx, results_fp);

    fclose(results_fp);
    close(sock_fd);
    free(tests);
    destroy_running_window();

    log_msg("Results written to %s\n", RESULTS_FILE);
    return rc;
}
