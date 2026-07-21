/*
 * xsimpleclip - Minimal ICCCM-compliant X11 clipboard image provider.
 *
 * Loads a PNG file, asserts CLIPBOARD ownership, and serves the image
 * via the INCR protocol when a requestor asks for image/png.
 * Logs every significant event to stdout with millisecond timestamps.
 * Exits after a successful paste or SelectionClear.
 *
 * Build:  make -f Makefile.linux
 * Usage:  ./xsimpleclip image.png
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ---------- logging ---------- */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

#define LOG(fmt, ...) \
    do { \
        fprintf(stdout, "[%12.3f] " fmt "\n", now_ms(), ##__VA_ARGS__); \
        fflush(stdout); \
        fprintf(stderr, "[%12.3f] " fmt "\n", now_ms(), ##__VA_ARGS__); \
        fflush(stderr); \
    } while(0)

/* ---------- atoms we intern ---------- */
static xcb_atom_t ATOM_CLIPBOARD;
static xcb_atom_t ATOM_TARGETS;
static xcb_atom_t ATOM_TIMESTAMP;
static xcb_atom_t ATOM_MULTIPLE;
static xcb_atom_t ATOM_IMAGE_PNG;
static xcb_atom_t ATOM_IMAGE_BMP;
static xcb_atom_t ATOM_IMAGE_JPEG;
static xcb_atom_t ATOM_IMAGE_GIF;
static xcb_atom_t ATOM_INCR;
static xcb_atom_t ATOM_UTF8_STRING;

/* ---------- image data ---------- */
static unsigned char *g_image_data = NULL;
static size_t        g_image_size = 0;
static const char   *g_filename   = NULL;  /* argv[1] for text mode         */

/* ---------- X11 state ---------- */
static xcb_connection_t *g_conn = NULL;
static xcb_window_t       g_win  = XCB_NONE;
static int                g_running = 1;

/* ---------- INCR transfer state ---------- */
static int         g_incr_active    = 0;    /* 1 while sending chunks          */
static int         g_incr_done      = 0;    /* 1 after final zero-length chunk */
static double      g_incr_done_time = 0;    /* when the transfer completed     */
static int         g_text_mode      = 0;    /* 0=image 1=countdown 2=text-copied */
static int         g_countdown_next = 0;    /* next phase# to print (1..5)       */
static int         g_text_served    = 0;    /* filename text sent to requestor   */
static xcb_window_t g_incr_requestor;
static xcb_atom_t   g_incr_property;
static xcb_atom_t   g_incr_selection;
static xcb_atom_t   g_incr_target;
static size_t       g_incr_offset;
static uint32_t     g_incr_chunk_size;
static size_t       g_incr_total;
static unsigned int g_incr_chunk_num;
static unsigned int g_incr_total_chunks;
static double       g_incr_start_time;
static double       g_incr_delete_wait_start;
static int          g_abort            = 0;    /* -abort: _exit(1) halfway through INCR */

/* ---------- paste mode state ---------- */
static int          g_paste_mode         = 0;
static const char  *g_output_file        = NULL;
static xcb_atom_t   g_paste_target       = XCB_NONE;
static unsigned char *g_paste_data       = NULL;
static size_t       g_paste_size         = 0;
static size_t       g_paste_alloc        = 0;
static int          g_paste_incr         = 0;
static size_t       g_paste_incr_expected = 0;
static xcb_atom_t   g_paste_property;

/* ---------- helpers ---------- */

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_cookie_t c = xcb_intern_atom(g_conn, 0, strlen(name), name);
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(g_conn, c, NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static const char *get_atom_name(xcb_atom_t atom)
{
    if (atom == ATOM_CLIPBOARD)   return "CLIPBOARD";
    if (atom == ATOM_TARGETS)     return "TARGETS";
    if (atom == ATOM_TIMESTAMP)   return "TIMESTAMP";
    if (atom == ATOM_MULTIPLE)    return "MULTIPLE";
    if (atom == ATOM_IMAGE_PNG)   return "image/png";
    if (atom == ATOM_IMAGE_BMP)   return "image/bmp";
    if (atom == ATOM_IMAGE_JPEG)  return "image/jpeg";
    if (atom == ATOM_IMAGE_GIF)   return "image/gif";
    if (atom == ATOM_INCR)        return "INCR";
    if (atom == XCB_ATOM_STRING)  return "STRING";
    if (atom == XCB_ATOM_NONE)    return "NONE";
    return "(unknown)";
}

static const char *prop_state_name(uint8_t state)
{
    switch (state) {
    case XCB_PROPERTY_NEW_VALUE: return "NEW_VALUE";
    case XCB_PROPERTY_DELETE:    return "DELETE";
    default:                     return "?";
    }
}

/* Send a SelectionNotify event.  property==NONE means conversion refused. */
static void send_selection_notify(xcb_window_t requestor,
                                  xcb_atom_t selection,
                                  xcb_atom_t target,
                                  xcb_atom_t property,
                                  xcb_timestamp_t time)
{
    xcb_selection_notify_event_t ev = {0};
    ev.response_type = XCB_SELECTION_NOTIFY;
    ev.requestor     = requestor;
    ev.selection     = selection;
    ev.target        = target;
    ev.property      = property;
    ev.time          = time;

    xcb_send_event(g_conn, 0, requestor, 0, (const char *)&ev);
    xcb_flush(g_conn);
}

/* ---------- INCR sender ---------- */

/* Forward declaration - called from incr_wait_for_delete */
static void incr_send_next(void);

/* Wait (polling) for the requestor to delete the property we wrote.
   When DELETE arrives, calls incr_send_next() to write the next chunk.
   Returns 1 to continue, 0 when INCR is done (or on error/timeout). */
static int incr_wait_for_delete(int timeout_ms)
{
    double deadline = now_ms() + (double)timeout_ms / 1000.0;

    while (now_ms() < deadline) {
        /* Poll events so we don't miss SelectionClear etc. */
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(g_conn))) {
            uint8_t rt = ev->response_type & ~0x80;
            if (rt == XCB_PROPERTY_NOTIFY) {
                xcb_property_notify_event_t *pn = (xcb_property_notify_event_t *)ev;
                if (pn->window == g_incr_requestor &&
                    pn->atom   == g_incr_property &&
                    pn->state  == XCB_PROPERTY_DELETE) {
                    double waited = now_ms() - g_incr_delete_wait_start;
                    double t = now_ms() - g_incr_start_time;
                    LOG("INCR: PropertyNotify(DELETE) [%u.%03u] - "
                        "waited %.3f ms for this chunk",
                        (unsigned)t, (unsigned)((t - (unsigned)t) * 1000.0),
                        waited);
                    free(ev);
                    incr_send_next();
                    if (!g_incr_active) return 0; /* transfer complete */
                    /* More chunks to send - reset timeout and keep
                       waiting for the next DELETE. */
                    deadline = now_ms() + (double)timeout_ms / 1000.0;
                    g_incr_delete_wait_start = now_ms();
                    continue;
                }
                LOG("PropertyNotify: window=0x%x atom=%s state=%s (ignored)",
                    (unsigned)pn->window, get_atom_name(pn->atom),
                    prop_state_name(pn->state));
            } else if (rt == XCB_SELECTION_CLEAR) {
                LOG("SelectionClear during INCR - aborting transfer");
                free(ev);
                return 0;
            } else if (rt == 0) {
                xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
                LOG("X11 error during INCR: code=%d major=%d minor=%d id=0x%x",
                    err->error_code, err->major_code, err->minor_code,
                    (unsigned)err->resource_id);
            }
            free(ev);
        }

        /* Check connection */
        if (xcb_connection_has_error(g_conn)) {
            LOG("INCR: X11 connection error during wait");
            return 0;
        }

        /* Small sleep to avoid busy-looping */
        usleep(5000); /* 5 ms */
    }

    LOG("INCR: TIMEOUT waiting for PropertyNotify(DELETE) after %d ms", timeout_ms);
    return 0;
}

/* Begin or resume the INCR transfer.  Called after each DELETE is seen. */
static void incr_send_next(void)
{
    if (!g_incr_active) return;

    size_t remaining = g_image_size - g_incr_offset;

    if (remaining == 0 && g_incr_chunk_num > 0) {
        /* All data sent - write zero-length final chunk (ICCCM end marker) */
        double t = now_ms() - g_incr_start_time;
        LOG("INCR: chunk %u/%u FINAL [%u.%03u] - writing zero-length terminator",
            g_incr_chunk_num + 1, g_incr_total_chunks,
            (unsigned)t, (unsigned)((t - (unsigned)t) * 1000.0));
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                            g_incr_requestor, g_incr_property,
                            g_incr_target, 8, 0, NULL);
        xcb_flush(g_conn);

        /* Send final SelectionNotify per ICCCM */
        send_selection_notify(g_incr_requestor, g_incr_selection,
                              g_incr_target, g_incr_property,
                              XCB_CURRENT_TIME);

        t = now_ms() - g_incr_start_time;
        LOG("INCR: transfer complete [%u.%03u] - %zu bytes in %u chunks",
            (unsigned)t, (unsigned)((t - (unsigned)t) * 1000.0),
            g_incr_total, g_incr_chunk_num);
        g_incr_active      = 0;
        g_incr_done        = 1;
        g_incr_done_time   = now_ms();
        g_countdown_next   = 1;
        g_text_mode        = 1;  /* enter countdown phase */
        return;
    }

    /* Work out chunk size */
    size_t chunk = remaining;
    if (chunk > g_incr_chunk_size)
        chunk = g_incr_chunk_size;

    g_incr_chunk_num++;

    double t = now_ms() - g_incr_start_time;
    LOG("INCR: chunk %u/%u [%u.%03u] - writing %zu bytes at offset %zu",
        g_incr_chunk_num, g_incr_total_chunks,
        (unsigned)t, (unsigned)((t - (unsigned)t) * 1000.0),
        chunk, g_incr_offset);

    /* ICCCM mandates PropModeAppend.  Each append triggers PropertyNotify
       (NEW_VALUE) on the requestor, which the receiver uses to wake up. */
    xcb_change_property(g_conn, XCB_PROP_MODE_APPEND,
                        g_incr_requestor, g_incr_property,
                        g_incr_target, 8, chunk,
                        g_image_data + g_incr_offset);
    xcb_flush(g_conn);

    /* ICCCM: after each append, send SelectionNotify */
    send_selection_notify(g_incr_requestor, g_incr_selection,
                          g_incr_target, g_incr_property,
                          XCB_CURRENT_TIME);

    g_incr_offset += chunk;

    if (g_abort && g_incr_offset >= g_image_size / 2) {
        LOG("INCR: ABORT at chunk %u/%u (offset %zu / %zu) - calling _exit(1)",
            g_incr_chunk_num, g_incr_total_chunks, g_incr_offset, g_image_size);
        _exit(1);
    }

    if (g_incr_offset >= g_image_size) {
        /* All chunks written - wait for final DELETE, then write zero-length */
        t = now_ms() - g_incr_start_time;
        LOG("INCR: all %zu data bytes sent [%u.%03u], "
            "waiting for final DELETE",
            g_image_size,
            (unsigned)t, (unsigned)((t - (unsigned)t) * 1000.0));
    }

    /* Record when we started waiting for the next DELETE */
    g_incr_delete_wait_start = now_ms();
}

/* Start an INCR transfer for a SelectionRequest */
static void incr_start(xcb_selection_request_event_t *e)
{
    uint32_t max_req = xcb_get_maximum_request_length(g_conn);
    uint32_t max_chunk = max_req * 4 - 24;
    /* Cap at 256 KB - same cap VcXsrv uses */
    if (max_chunk > 262144)
        max_chunk = 262144;

    g_incr_active        = 1;
    g_incr_done          = 0;
    g_incr_start_time    = now_ms();
    g_incr_requestor     = e->requestor;
    g_incr_property      = e->property;
    g_incr_selection     = e->selection;
    g_incr_target        = e->target;
    g_incr_offset        = 0;
    g_incr_chunk_size    = max_chunk;
    g_incr_total         = g_image_size;
    g_incr_chunk_num     = 0;
    g_incr_total_chunks  = (unsigned int)((g_image_size + max_chunk - 1)
                                         / max_chunk);

    LOG("--- INCR TRANSFER START ---");
    LOG("INCR: image_size=%zu max_chunk=%u total_chunks=%u requestor=0x%x",
        g_image_size, max_chunk, g_incr_total_chunks,
        (unsigned)e->requestor);
    LOG("INCR: target=%s property=0x%x selection=%s",
        get_atom_name(e->target), (unsigned)e->property,
        get_atom_name(e->selection));

    /* Select PropertyChangeMask on the requestor window so we receive
       PropertyNotify(DELETE) events when the receiver deletes each chunk. */
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t val  = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(g_conn, e->requestor, mask, &val);
    xcb_flush(g_conn);
    LOG("INCR: selected PropertyChangeMask on requestor window 0x%x",
        (unsigned)e->requestor);

    /* Step 1: INCR start property.
       ICCCM: SelectionNotify.target == actual conversion target (image/png).
       The property type is INCR to signal incremental transfer. */
    uint32_t lower_bound = (uint32_t)g_image_size;
    xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                        e->requestor, e->property,
                        ATOM_INCR, 32, 1, &lower_bound);
    send_selection_notify(e->requestor, e->selection,
                          e->target, e->property, e->time);
    LOG("INCR: sent start property (type=INCR, lower_bound=%" PRIu32 ")", lower_bound);

    /* Now wait for the first DELETE before sending the first data chunk.
       The receiver must delete the INCR-start property to signal readiness. */
    g_incr_delete_wait_start = now_ms();
    LOG("INCR: waiting for first DELETE (receiver acknowledges INCR start)...");
}

/* ---------- Selection request handler ---------- */

static void handle_selection_request(xcb_selection_request_event_t *e)
{
    LOG("SelectionRequest: requestor=0x%x selection=%s target=%s property=0x%x time=%u",
        (unsigned)e->requestor,
        get_atom_name(e->selection),
        get_atom_name(e->target),
        (unsigned)e->property,
        (unsigned)e->time);

    /* Reject requests for selections other than CLIPBOARD */
    if (e->selection != ATOM_CLIPBOARD) {
        LOG("SelectionRequest: refusing - we only own CLIPBOARD, not %s",
            get_atom_name(e->selection));
        send_selection_notify(e->requestor, e->selection, e->target,
                              XCB_NONE, e->time);
        return;
    }

    /* TARGETS - list what we can convert to */
    if (e->target == ATOM_TARGETS) {
        xcb_atom_t targets[8];
        int n = 0;
        targets[n++] = ATOM_TARGETS;
        targets[n++] = ATOM_TIMESTAMP;
        targets[n++] = ATOM_MULTIPLE;
        targets[n++] = ATOM_IMAGE_PNG;
        targets[n++] = ATOM_IMAGE_BMP;
        targets[n++] = ATOM_IMAGE_JPEG;
        targets[n++] = ATOM_IMAGE_GIF;

        {
            char tgtlist[256];
            int off = 0, i;
            for (i = 0; i < n; i++)
                off += snprintf(tgtlist + off, sizeof(tgtlist) - off, "%s%s",
                                i ? " " : " [", get_atom_name(targets[i]));
            snprintf(tgtlist + off, sizeof(tgtlist) - off, "]");
            LOG("SelectionRequest TARGETS: offering %d formats%s", n, tgtlist);
        }
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                            e->requestor, e->property,
                            XCB_ATOM_ATOM, 32, n, targets);
        send_selection_notify(e->requestor, e->selection,
                              e->target, e->property, e->time);
        return;
    }

    /* TIMESTAMP */
    if (e->target == ATOM_TIMESTAMP) {
        xcb_timestamp_t ts = e->time;
        LOG("SelectionRequest TIMESTAMP: returning %u", (unsigned)ts);
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                            e->requestor, e->property,
                            XCB_ATOM_INTEGER, 32, 1, &ts);
        send_selection_notify(e->requestor, e->selection,
                              e->target, e->property, e->time);
        return;
    }

    /* MULTIPLE - not implemented (rarely used) */
    if (e->target == ATOM_MULTIPLE) {
        LOG("SelectionRequest MULTIPLE: refusing (not implemented)");
        send_selection_notify(e->requestor, e->selection, e->target,
                              XCB_NONE, e->time);
        return;
    }

    /* Image targets */
    if (e->target == ATOM_IMAGE_PNG ||
        e->target == ATOM_IMAGE_BMP ||
        e->target == ATOM_IMAGE_JPEG ||
        e->target == ATOM_IMAGE_GIF) {

        if (!g_image_data || g_image_size == 0) {
            LOG("SelectionRequest %s: no image data loaded, refusing",
                get_atom_name(e->target));
            send_selection_notify(e->requestor, e->selection, e->target,
                                  XCB_NONE, e->time);
            return;
        }

        LOG("SelectionRequest %s: image is %zu bytes",
            get_atom_name(e->target), g_image_size);

        /* Cap at 256 KB to force INCR for larger images */
        uint32_t max_size = 262144;

        if (g_image_size <= max_size) {
            /* Small image - direct write, no INCR */
            LOG("SelectionRequest %s: fits in single request (%zu <= %u), "
                "direct write",
                get_atom_name(e->target), g_image_size, max_size);
            xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                                e->requestor, e->property,
                                e->target, 8, g_image_size, g_image_data);
            send_selection_notify(e->requestor, e->selection,
                                  e->target, e->property, e->time);
            LOG("Direct transfer complete: %zu bytes", g_image_size);
            /* Start countdown → text-mode exit, same as INCR path */
            g_incr_done        = 1;
            g_incr_done_time   = now_ms();
            g_countdown_next   = 1;
            g_text_mode        = 1;
        } else {
            /* Large image - INCR protocol */
            LOG("SelectionRequest %s: too large for single request "
                "(%zu > %u), starting INCR",
                get_atom_name(e->target), g_image_size, max_size);
            incr_start(e);
        }
        return;
    }

    /* Text mode: after image INCR completes we switch to offering the
       filename.  Non-INCR since filenames are always small. */
    if (g_text_mode == 2 && g_filename &&
        (e->target == XCB_ATOM_STRING ||
         e->target == ATOM_UTF8_STRING)) {
        size_t len = strlen(g_filename);
        LOG("SelectionRequest %s: serving filename \"%s\" (%zu bytes, "
            "non-INCR)",
            get_atom_name(e->target), g_filename, len);
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                            e->requestor, e->property,
                            e->target, 8, len, g_filename);
        send_selection_notify(e->requestor, e->selection,
                              e->target, e->property, e->time);
        g_text_served = 1;
        return;
    }

    /* Text-mode TARGETS */
    if (g_text_mode == 2 && e->target == ATOM_TARGETS) {
        xcb_atom_t targets[4];
        int n = 0;
        targets[n++] = ATOM_TARGETS;
        targets[n++] = ATOM_TIMESTAMP;
        targets[n++] = XCB_ATOM_STRING;
        targets[n++] = ATOM_UTF8_STRING;
        {
            char tgtlist[256];
            int off = 0, i;
            for (i = 0; i < n; i++)
                off += snprintf(tgtlist + off, sizeof(tgtlist) - off, "%s%s",
                                i ? " " : " [", get_atom_name(targets[i]));
            snprintf(tgtlist + off, sizeof(tgtlist) - off, "]");
            LOG("SelectionRequest TARGETS (text mode): offering %d formats%s",
                n, tgtlist);
        }
        xcb_change_property(g_conn, XCB_PROP_MODE_REPLACE,
                            e->requestor, e->property,
                            XCB_ATOM_ATOM, 32, n, targets);
        send_selection_notify(e->requestor, e->selection,
                              e->target, e->property, e->time);
        return;
    }

    /* Unknown target - refuse */
    LOG("SelectionRequest: refusing unsupported target %s",
        get_atom_name(e->target));
    send_selection_notify(e->requestor, e->selection, e->target,
                          XCB_NONE, e->time);
}

/* ---------- main event loop ---------- */

static void main_loop(void)
{
    int fd = xcb_get_file_descriptor(g_conn);

    while (g_running) {
        /* Drain any queued events first */
        xcb_generic_event_t *ev;

        while ((ev = xcb_poll_for_event(g_conn))) {
            uint8_t rt = ev->response_type & ~0x80;

            switch (rt) {

            case XCB_SELECTION_REQUEST: {
                xcb_selection_request_event_t *sr =
                    (xcb_selection_request_event_t *)ev;

                /* If we are mid-INCR and a new request arrives for the same
                   selection, abort the old transfer. */
                if (g_incr_active) {
                    LOG("New SelectionRequest while INCR active - aborting "
                        "previous transfer");
                    g_incr_active = 0;
                }
                handle_selection_request(sr);
                break;
            }

            case XCB_SELECTION_CLEAR: {
                xcb_selection_clear_event_t *sc =
                    (xcb_selection_clear_event_t *)ev;
                LOG("SelectionClear: selection=%s time=%u",
                    get_atom_name(sc->selection), (unsigned)sc->time);

                if (g_incr_active) {
                    LOG("SelectionClear while INCR active - "
                        "transfer aborted");
                    g_incr_active = 0;
                }

                if (g_incr_done || g_text_mode) {
                    LOG("SelectionClear - exiting");
                    g_running = 0;
                } else if (!g_incr_active) {
                    LOG("SelectionClear without paste - exiting");
                    g_running = 0;
                }
                break;
            }

            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t *pn =
                    (xcb_property_notify_event_t *)ev;

                LOG("PropertyNotify: window=0x%x atom=%s state=%s",
                    (unsigned)pn->window,
                    get_atom_name(pn->atom),
                    prop_state_name(pn->state));

                /* INCR: check for DELETE on the requestor window */
                if (g_incr_active &&
                    pn->window == g_incr_requestor &&
                    pn->atom   == g_incr_property &&
                    pn->state  == XCB_PROPERTY_DELETE) {
                    incr_send_next();
                }
                break;
            }

            case 0: {
                xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
                LOG("X11 error: code=%d major=%d minor=%d id=0x%x",
                    err->error_code, err->major_code, err->minor_code,
                    (unsigned)err->resource_id);
                if (g_incr_active && g_incr_chunk_num > 0) {
                    LOG("Error during INCR - aborting transfer");
                    g_incr_active = 0;
                }
                break;
            }

            default:
                LOG("Unhandled event: response_type=%u",
                    (unsigned)rt);
                break;
            }

            free(ev);

            /* Check connection health */
            if (xcb_connection_has_error(g_conn)) {
                LOG("X11 connection error - exiting");
                g_running = 0;
                break;
            }
        }

        if (!g_running) break;

        /* If INCR is active, poll for PropertyNotify(DELETE) from the
           requestor.  incr_wait_for_delete() calls incr_send_next()
           internally when a DELETE arrives. */
        if (g_incr_active) {
            incr_wait_for_delete(30000);
            if (g_incr_done || !g_incr_active) continue;
            LOG("INCR timeout waiting for DELETE - aborting transfer");
            g_incr_active = 0;
            continue;
        }

        /* Countdown after INCR completes: print "waiting to exit N/5"
           every 200 ms for 1 second, then switch clipboard to the
           filename as text and exit. */
        if (g_text_mode == 1) {
            double elapsed = now_ms() - g_incr_done_time;
            int phase = (int)(elapsed / 0.200) + 1;
            if (phase > 5) phase = 5;

            if (phase >= g_countdown_next) {
                LOG("waiting to exit %d/5", phase);
                g_countdown_next = phase + 1;

                if (phase >= 5) {
                    /* Re-take CLIPBOARD ownership - this signals VcXsrv
                       that clipboard content has changed to text. */
                    xcb_void_cookie_t ck = xcb_set_selection_owner_checked(
                        g_conn, g_win, ATOM_CLIPBOARD, XCB_CURRENT_TIME);
                    xcb_generic_error_t *err =
                        xcb_request_check(g_conn, ck);
                    if (err) free(err);
                    xcb_flush(g_conn);
                    g_text_mode = 2;
                    LOG("TEXT-COPY: filename \"%s\" now on CLIPBOARD, "
                        "exiting", g_filename);
                    g_running = 0;
                }
            }
        }

        /* Text mode: after filename has been served to a requestor,
           exit immediately. */
        if (g_text_mode == 2 && g_text_served) {
            LOG("Text served, exiting");
            g_running = 0;
        }

        /* Block briefly (100 ms) so the countdown fires promptly.
           When nothing is in progress this acts as a heartbeat. */
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = {0, 100000};
            select(fd + 1, &fds, NULL, NULL, &tv);
        }
    }
}

/* ---------- paste mode ---------- */

static const char *ext_to_target(const char *path, xcb_atom_t *atom_out)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return NULL;
    if (strcasecmp(ext, ".png") == 0)  { *atom_out = ATOM_IMAGE_PNG;  return "image/png"; }
    if (strcasecmp(ext, ".bmp") == 0)  { *atom_out = ATOM_IMAGE_BMP;  return "image/bmp"; }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
                                        { *atom_out = ATOM_IMAGE_JPEG; return "image/jpeg"; }
    if (strcasecmp(ext, ".gif") == 0)  { *atom_out = ATOM_IMAGE_GIF;  return "image/gif"; }
    return NULL;
}

static void paste_append_data(const unsigned char *data, size_t len)
{
    if (g_paste_size + len > g_paste_alloc) {
        size_t new_alloc = g_paste_alloc ? g_paste_alloc * 2 : 262144;
        if (new_alloc < g_paste_size + len)
            new_alloc = g_paste_size + len;
        unsigned char *new_data = realloc(g_paste_data, new_alloc);
        if (!new_data) {
            LOG("paste: realloc failed");
            g_running = 0;
            return;
        }
        g_paste_data = new_data;
        g_paste_alloc = new_alloc;
    }
    memcpy(g_paste_data + g_paste_size, data, len);
    g_paste_size += len;
}

static void paste_write_file(void)
{
    FILE *fp = fopen(g_output_file, "wb");
    if (!fp) {
        LOG("paste: cannot open %s for writing", g_output_file);
        return;
    }
    size_t written = fwrite(g_paste_data, 1, g_paste_size, fp);
    fclose(fp);
    if (written != g_paste_size)
        LOG("paste: write truncated: %zu of %zu bytes", written, g_paste_size);
    else
        LOG("paste: wrote %zu bytes to %s", g_paste_size, g_output_file);
}

static void paste_read_property(void)
{
    xcb_get_property_cookie_t ck = xcb_get_property(g_conn, 0, g_win,
        g_paste_property, XCB_ATOM_ANY, 0, 0xFFFFFFFFul);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(g_conn, ck, NULL);
    if (!reply) {
        LOG("paste: get_property_reply failed");
        g_running = 0;
        return;
    }

    LOG("paste: read property type=%u format=%u len=%u bytes_after=%u",
        (unsigned)reply->type, (unsigned)reply->format,
        (unsigned)xcb_get_property_value_length(reply),
        (unsigned)reply->bytes_after);

    if (reply->type == ATOM_INCR) {
        g_paste_incr = 1;
        g_paste_incr_expected = *(uint32_t *)xcb_get_property_value(reply);
        g_paste_size = 0;
        LOG("paste: INCR start, expected %zu bytes", g_paste_incr_expected);
        free(reply);
        xcb_delete_property(g_conn, g_win, g_paste_property);
        xcb_flush(g_conn);
        return;
    }

    int len = xcb_get_property_value_length(reply);
    const unsigned char *value = xcb_get_property_value(reply);

    if (len == 0) {
        if (g_paste_incr)
            LOG("paste: INCR complete, %zu bytes received", g_paste_size);
        free(reply);
        paste_write_file();
        g_running = 0;
        return;
    }

    paste_append_data(value, len);

    if (g_paste_incr) {
        LOG("paste: INCR chunk +%d = %zu bytes", len, g_paste_size);
        free(reply);
        xcb_delete_property(g_conn, g_win, g_paste_property);
        xcb_flush(g_conn);
    } else {
        LOG("paste: direct transfer, %zu bytes", g_paste_size);
        free(reply);
        paste_write_file();
        g_running = 0;
    }
}

static void paste_request(void)
{
    xcb_convert_selection(g_conn, g_win, ATOM_CLIPBOARD,
                          g_paste_target, g_paste_property, XCB_CURRENT_TIME);
    xcb_flush(g_conn);
}

static void paste_loop(void)
{
    int fd = xcb_get_file_descriptor(g_conn);
    const char *target_name = ext_to_target(g_output_file, &g_paste_target);

    if (!g_paste_target) {
        LOG("paste: unknown extension for %s (need .png .bmp .jpg .jpeg .gif)",
            g_output_file);
        return;
    }
    LOG("paste: requesting CLIPBOARD as %s, saving to %s",
        target_name, g_output_file);

    g_paste_property = intern("XSIMPLE_PASTE");

    paste_request();

    while (g_running) {
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(g_conn))) {
            uint8_t rt = ev->response_type & ~0x80;

            switch (rt) {
            case XCB_SELECTION_NOTIFY: {
                xcb_selection_notify_event_t *sn =
                    (xcb_selection_notify_event_t *)ev;
                LOG("paste: SelectionNotify requestor=0x%x selection=%u target=%u property=%u time=%u",
                    (unsigned)sn->requestor, (unsigned)sn->selection,
                    (unsigned)sn->target, (unsigned)sn->property,
                    (unsigned)sn->time);
                if (sn->property == XCB_NONE) {
                    LOG("paste: owner refused conversion to %s", target_name);
                    g_running = 0;
                } else if (!g_paste_incr) {
                    paste_read_property();
                }
                break;
            }
            case XCB_PROPERTY_NOTIFY: {
                xcb_property_notify_event_t *pn =
                    (xcb_property_notify_event_t *)ev;
                LOG("paste: PropertyNotify window=0x%x atom=%u state=%u",
                    (unsigned)pn->window, (unsigned)pn->atom,
                    (unsigned)pn->state);
                if (pn->window == g_win && pn->atom == g_paste_property &&
                    pn->state == XCB_PROPERTY_NEW_VALUE) {
                    paste_read_property();
                }
                break;
            }
            case XCB_SELECTION_CLEAR:
                LOG("paste: SelectionClear - owner went away");
                g_running = 0;
                break;
            case 0: {
                xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
                LOG("paste: X11 error code=%d", err->error_code);
                g_running = 0;
                break;
            }
            default:
                LOG("paste: unhandled event response_type=%u", (unsigned)rt);
                break;
            }
            free(ev);
            if (!g_running) break;
            if (xcb_connection_has_error(g_conn)) {
                LOG("paste: connection error");
                g_running = 0;
                break;
            }
        }

        if (!g_running) break;

        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            struct timeval tv = {0, 100000};
            select(fd + 1, &fds, NULL, NULL, &tv);
        }
    }

    free(g_paste_data);
    g_paste_data = NULL;
}

/* ---------- initialisation ---------- */

static int setup_atoms(void)
{
    ATOM_CLIPBOARD  = intern("CLIPBOARD");
    ATOM_TARGETS    = intern("TARGETS");
    ATOM_TIMESTAMP  = intern("TIMESTAMP");
    ATOM_MULTIPLE   = intern("MULTIPLE");
    ATOM_IMAGE_PNG  = intern("image/png");
    ATOM_IMAGE_BMP  = intern("image/bmp");
    ATOM_IMAGE_JPEG = intern("image/jpeg");
    ATOM_IMAGE_GIF  = intern("image/gif");
    ATOM_INCR        = intern("INCR");
    ATOM_UTF8_STRING = intern("UTF8_STRING");

    if (!ATOM_CLIPBOARD || !ATOM_TARGETS || !ATOM_TIMESTAMP ||
        !ATOM_IMAGE_PNG || !ATOM_INCR) {
        LOG("FATAL: failed to intern required atoms");
        return 0;
    }

    LOG("Atoms interned: CLIPBOARD=%u TARGETS=%u INCR=%u image/png=%u",
        (unsigned)ATOM_CLIPBOARD, (unsigned)ATOM_TARGETS,
        (unsigned)ATOM_INCR, (unsigned)ATOM_IMAGE_PNG);
    return 1;
}

int main(int argc, char **argv)
{
    int w, h, channels;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-abort] <image.png>\n"
                "       %s -paste <output.png|.bmp|.jpg|.gif>\n",
                argv[0], argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-paste") == 0) {
        g_paste_mode = 1;
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -paste <output.png|.bmp|.jpg|.gif>\n",
                    argv[0]);
            return 1;
        }
        g_output_file = argv[2];
    } else if (strcmp(argv[1], "-abort") == 0) {
        g_abort = 1;
        LOG("ABORT mode: will _exit(1) halfway through INCR transfer");
        if (argc < 3) {
            fprintf(stderr, "Usage: %s [-abort] <image.png>\n", argv[0]);
            return 1;
        }
        g_filename = argv[2];
    } else {
        g_filename = argv[1];
    }

    if (!g_paste_mode) {
        /* Load PNG into memory.
           We keep the original PNG bytes so we can serve image/png verbatim.
           stb_image also gives us decoded info for diagnostic logging. */
        g_image_data = stbi_load(g_filename, &w, &h, &channels, 0);
        if (!g_image_data) {
            /* Not decodable as pixels - try loading as raw file for serving
               image/png verbatim (the file itself is the PNG bytestream). */
            FILE *fp = fopen(g_filename, "rb");
            if (!fp) {
                fprintf(stderr, "Cannot open %s\n", g_filename);
                return 1;
            }
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            g_image_data = malloc(sz);
            if (!g_image_data) {
                fprintf(stderr, "malloc(%ld) failed\n", sz);
                fclose(fp);
                return 1;
            }
            g_image_size = sz;
            if (fread(g_image_data, 1, sz, fp) != (size_t)sz) {
                fprintf(stderr, "fread failed\n");
                fclose(fp);
                return 1;
            }
            fclose(fp);
            LOG("Loaded PNG file as raw bytes: %zu bytes", g_image_size);
        } else {
            /* stb_image decoded it - but for clipboard we need the original
               PNG bytestream, not decoded pixels.  Load the raw file alongside. */
            LOG("Decoded PNG: %dx%d channels=%d", w, h, channels);
            stbi_image_free(g_image_data);
            g_image_data = NULL;

            FILE *fp = fopen(g_filename, "rb");
            if (!fp) {
                fprintf(stderr, "Cannot re-open %s\n", g_filename);
                return 1;
            }
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            g_image_data = malloc(sz);
            if (!g_image_data) {
                fprintf(stderr, "malloc(%ld) failed\n", sz);
                fclose(fp);
                return 1;
            }
            g_image_size = sz;
            if (fread(g_image_data, 1, sz, fp) != (size_t)sz) {
                fprintf(stderr, "fread failed\n");
                fclose(fp);
                free(g_image_data);
                g_image_data = NULL;
                return 1;
            }
            fclose(fp);
            LOG("Loaded PNG file as raw bytes: %zu bytes (%dx%d)",
                g_image_size, w, h);
        }
    }

    /* Connect to X server */
    g_conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(g_conn)) {
        fprintf(stderr, "Cannot connect to X server\n");
        return 1;
    }
    LOG("Connected to X server (fd=%d)", xcb_get_file_descriptor(g_conn));

    /* Intern atoms */
    if (!setup_atoms()) return 1;

    /* Create a small off-screen window (we never map it).
       We need a window to own the selection and receive events. */
    xcb_screen_t *screen = xcb_setup_roots_iterator(
        xcb_get_setup(g_conn)).data;
    g_win = xcb_generate_id(g_conn);
    uint32_t win_mask = XCB_CW_EVENT_MASK;
    uint32_t win_vals[] = {
        XCB_EVENT_MASK_PROPERTY_CHANGE  /* PropertyNotify for INCR */
    };
    xcb_create_window(g_conn,
                      XCB_COPY_FROM_PARENT,        /* depth   */
                      g_win,                        /* window  */
                      screen->root,                 /* parent  */
                      0, 0, 1, 1, 0,                /* x,y,w,h,border */
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual,
                      win_mask, win_vals);
    xcb_flush(g_conn);
    LOG("Created window 0x%x", (unsigned)g_win);

    if (g_paste_mode) {
        paste_loop();
    } else {
        /* Take ownership of CLIPBOARD selection.
           This is the X11 equivalent of "Copy". */
        xcb_void_cookie_t ck = xcb_set_selection_owner_checked(
            g_conn, g_win, ATOM_CLIPBOARD, XCB_CURRENT_TIME);
        xcb_generic_error_t *err = xcb_request_check(g_conn, ck);
        if (err) {
            LOG("FATAL: xcb_set_selection_owner failed: error_code=%d",
                err->error_code);
            free(err);
            return 1;
        }
        xcb_flush(g_conn);

        /* Verify ownership */
        xcb_get_selection_owner_cookie_t own_ck =
            xcb_get_selection_owner(g_conn, ATOM_CLIPBOARD);
        xcb_get_selection_owner_reply_t *own_r =
            xcb_get_selection_owner_reply(g_conn, own_ck, NULL);
        if (!own_r || own_r->owner != g_win) {
            LOG("FATAL: failed to take CLIPBOARD ownership "
                "(owner=0x%x, expected 0x%x)",
                own_r ? (unsigned)own_r->owner : 0, (unsigned)g_win);
            free(own_r);
            return 1;
        }
        free(own_r);
        LOG("TAKEN ownership of CLIPBOARD selection - %zu bytes of image/png "
            "available", g_image_size);
        LOG("Waiting for paste requests...");

        /* Event loop - runs until paste completes and SelectionClear arrives */
        main_loop();
    }

    LOG("Exiting.");
    free(g_image_data);
    xcb_disconnect(g_conn);
    return 0;
}
