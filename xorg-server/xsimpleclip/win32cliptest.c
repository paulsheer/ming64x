/*
 * win32cliptest.c - Windows clipboard test server.
 *
 * Listens on TCP port 21407, accepts one connection at a time, and processes
 * clipboard test requests from x11cliptest (Linux).
 *
 * Cross-compile: x86_64-w64-mingw32-gcc -o win32cliptest.exe win32cliptest.c -lws2_32
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "cliptest.h"

/* Hidden window for clipboard operations (OpenClipboard with NULL can
   cause SetClipboardData to fail on some Windows versions). */
static HWND g_hwnd = NULL;

static LRESULT CALLBACK clip_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* Clipboard format IDs — VcXsrv hardcodes specific numbers that were
   valid on the developer's machine but are NOT globally consistent.
   We also call RegisterClipboardFormat to get the system-correct numbers
   and advertise both so VcXsrv finds them regardless. */
static UINT fmt_vcx_png  = 0;
static UINT fmt_vcx_gif  = 0;
static UINT fmt_vcx_jfif = 0;

static void init_format_ids(void)
{
    fmt_vcx_png  = RegisterClipboardFormatA("PNG");
    fmt_vcx_gif  = RegisterClipboardFormatA("GIF");
    fmt_vcx_jfif = RegisterClipboardFormatA("JFIF");
}

#define LISTEN_PORT    21407
#define INPUT_DIR      "input"
#define MAX_IMAGE_SIZE (128 * 1024 * 1024)

#ifndef LCS_sRGB
#define LCS_sRGB 0x73524742  /* 'sRGB' */
#endif

#ifndef BI_BITFIELDS
#define BI_BITFIELDS 3
#endif

/* DROPFILES struct for CF_HDROP (not always in mingw-w64 shellapi.h) */
typedef struct {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} DROPFILES;

/* ---------- logging ---------- */
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fflush(stderr);
    va_end(ap);
}

/* ---------- socket helpers ---------- */

static int send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n == SOCKET_ERROR) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n == SOCKET_ERROR || n == 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

/* ---------- DIB construction ---------- */

/*
 * Build a packed CF_DIB (BITMAPINFOHEADER + pixels, bottom-up BGRA).
 * Returns malloc'd buffer, sets *out_len.
 */
static void *build_dib(int w, int h, const unsigned char *rgba,
                       int alpha, int v5, size_t *out_len)
{
    /* DIBV5 with transparency: output 32-bit BGRA with alpha preserved.
       Regular DIB or no transparency: 24-bit BGR blended against gray. */
    int has_alpha = (v5 && alpha == CLIPTEST_TRANSPARENCY_YES);
    int bpp       = has_alpha ? 4 : 3;
    int hdr_size  = v5 ? 124 : 40;
    int row_bytes = ((w * bpp + 3) / 4) * 4;
    int img_size  = row_bytes * h;
    size_t total  = hdr_size + img_size;
    unsigned char *dib = (unsigned char *)malloc(total);
    if (!dib) return NULL;

    memset(dib, 0, hdr_size);

    *(uint32_t *)(dib +  0) = hdr_size;
    *(int32_t  *)(dib +  4) = w;
    *(int32_t  *)(dib +  8) = h;               /* positive = bottom-up */
    *(uint16_t *)(dib + 12) = 1;
    *(uint16_t *)(dib + 14) = bpp * 8;         /* biBitCount: 24 or 32 */
    *(uint32_t *)(dib + 16) = has_alpha ? BI_BITFIELDS : BI_RGB;
    *(uint32_t *)(dib + 20) = img_size;

    if (v5) {
        if (has_alpha) {
            *(uint32_t *)(dib + 40) = 0x00FF0000;  /* bV5RedMask */
            *(uint32_t *)(dib + 44) = 0x0000FF00;  /* bV5GreenMask */
            *(uint32_t *)(dib + 48) = 0x000000FF;  /* bV5BlueMask */
            *(uint32_t *)(dib + 52) = 0xFF000000;  /* bV5AlphaMask */
        } else {
            *(uint32_t *)(dib + 40) = 0x00FF0000;  /* bV5RedMask */
            *(uint32_t *)(dib + 44) = 0x0000FF00;  /* bV5GreenMask */
            *(uint32_t *)(dib + 48) = 0x000000FF;  /* bV5BlueMask */
            *(uint32_t *)(dib + 52) = 0x00000000;  /* bV5AlphaMask */
        }
        *(uint32_t *)(dib + 56) = LCS_sRGB;
    }

    unsigned char *dst = dib + hdr_size;
    for (int y = 0; y < h; y++) {
        int src_y = h - 1 - y;
        for (int x = 0; x < w; x++) {
            int src_idx = (src_y * w + x) * 4;
            int dst_idx = y * row_bytes + x * bpp;
            unsigned int a = rgba[src_idx + 3];
            if (has_alpha) {
                dst[dst_idx + 0] = rgba[src_idx + 2];  /* B */
                dst[dst_idx + 1] = rgba[src_idx + 1];  /* G */
                dst[dst_idx + 2] = rgba[src_idx + 0];  /* R */
                dst[dst_idx + 3] = (unsigned char)a;    /* A */
            } else {
                unsigned int inv_a = 255 - a;
                dst[dst_idx + 0] = (unsigned char)
                    ((rgba[src_idx + 2] * a + 128 * inv_a + 127) / 255);
                dst[dst_idx + 1] = (unsigned char)
                    ((rgba[src_idx + 1] * a + 128 * inv_a + 127) / 255);
                dst[dst_idx + 2] = (unsigned char)
                    ((rgba[src_idx + 0] * a + 128 * inv_a + 127) / 255);
            }
        }
        memset(dst + y * row_bytes + w * bpp, 0, row_bytes - w * bpp);
    }

    *out_len = total;
    return dib;
}

/* ---------- message handling ---------- */

/* ---------- message handling ---------- */

static void build_image_path(const struct cliptest *ct, uint32_t fmt,
                              char *path, size_t path_sz)
{
    const char *sz = size_name(ct->width);
    const char *mc = (ct->transparency == CLIPTEST_TRANSPARENCY_YES) ? "circle" : "square";
    const char *ext = format_ext(fmt);
    snprintf(path, path_sz, INPUT_DIR "/dalmatian-%s-%s.%s", sz, mc, ext);
}

/*
 * Handle a single test request.  Returns 0 on success, -1 on error,
 * -2 when MSG_QUIT is received (server should exit).
 */
static int handle_request(SOCKET client)
{
    struct cliptest ct;
    unsigned char *image_out = NULL;
    size_t image_out_len = 0;
    char resp_ext[8] = {0};
    int ret = -1;

    /* Read header: type + length + index (12 bytes) */
    uint32_t hdr[3];
    if (recv_all(client, hdr, 12) < 0) {
        log_msg("Failed to receive request header\n");
        return -1;
    }

    uint32_t req_type   = hdr[0];
    uint32_t req_length = hdr[1];
    uint32_t req_index  = hdr[2];

    /* Clean exit request - no payload, just the 12-byte header */
    if (req_type == MSG_QUIT) {
        log_msg("RECV type=MSG_QUIT len=%u index=%u\n",
                (unsigned)req_length, (unsigned)req_index);
        return -2;
    }

    if (req_type != MSG_REQUEST) {
        log_msg("RECV type=%u(unknown) len=%u index=%u - expected MSG_REQUEST\n",
                (unsigned)req_type, (unsigned)req_length, (unsigned)req_index);
        return -1;
    }
    if (req_length < CLIPTEST_REQUEST_BASE || req_length > MAX_IMAGE_SIZE + CLIPTEST_REQUEST_BASE) {
        log_msg("RECV type=MSG_REQUEST len=%u index=%u - invalid length\n",
                (unsigned)req_length, (unsigned)req_index);
        return -1;
    }

    uint32_t payload_len = req_length - CLIPTEST_HEADER_SIZE;

    /* Read cliptest struct + optional image data */
    unsigned char *payload = (unsigned char *)malloc(payload_len);
    if (!payload) {
        log_msg("RECV type=MSG_REQUEST len=%u index=%u - malloc failed\n",
                (unsigned)req_length, (unsigned)req_index);
        return -1;
    }
    if (recv_all(client, payload, payload_len) < 0) {
        log_msg("RECV type=MSG_REQUEST len=%u index=%u - payload read failed\n",
                (unsigned)req_length, (unsigned)req_index);
        free(payload);
        return -1;
    }

    /* Parse cliptest struct */
    if (payload_len < CLIPTEST_STRUCT_SIZE) {
        log_msg("RECV type=MSG_REQUEST len=%u index=%u - payload too small\n",
                (unsigned)req_length, (unsigned)req_index);
        free(payload);
        return -1;
    }
    memcpy(&ct, payload, CLIPTEST_STRUCT_SIZE);

    log_msg("RECV type=MSG_REQUEST len=%u index=%u dir=%s from=%s to=%s size=%dx%d hdrop=%s transp=%s imgsize=%u\n",
            (unsigned)req_length, (unsigned)req_index,
            direction_name(ct.direction),
            format_name(ct.from_format), format_name(ct.to_format),
            (int)ct.width, (int)ct.height,
            hdrop_name(ct.hdrop),
            transparency_name(ct.transparency),
            (unsigned)(payload_len - CLIPTEST_STRUCT_SIZE));

    /* Process the test */
    if (ct.direction == CLIPTEST_WIN32_TO_X11) {
        /* Win32 is Copy source: load PNG, put on Windows clipboard.
           VcXsrv will detect the clipboard change and reassert X11
           selection ownership.  The Linux side then requests the
           X11 selection and receives the converted image through
           the X server — we never send image data on the socket. */

        /* Load the PNG source */
        char png_path[512];
        build_image_path(&ct, CLIPTEST_FORMAT_PNG, png_path, sizeof(png_path));
        log_msg("RECV index=%u - WIN32_TO_X11: loading %s\n",
                (unsigned)req_index, png_path);

        if (!OpenClipboard(g_hwnd)) {
            log_msg("RECV index=%u - OpenClipboard failed: %lu\n",
                    (unsigned)req_index, GetLastError());
            goto send_response;
        }

        if (!EmptyClipboard()) {
            log_msg("RECV index=%u - EmptyClipboard failed: %lu\n",
                    (unsigned)req_index, GetLastError());
            CloseClipboard();
            goto send_response;
        }

        /* Put actual text data (not NULL) so Windows doesn't expect
           delayed rendering from our DefWindowProc-only window. */
        {
            const char *txt = "cliptest";
            int txt_len = (int)strlen(txt) + 1;
            HGLOBAL hTextU = GlobalAlloc(GMEM_MOVEABLE, txt_len * 2);
            if (hTextU) {
                wchar_t *wu = (wchar_t *)GlobalLock(hTextU);
                if (wu) {
                    MultiByteToWideChar(CP_UTF8, 0, txt, -1, wu, txt_len);
                    GlobalUnlock(hTextU);
                    SetClipboardData(CF_UNICODETEXT, hTextU);
                } else { GlobalFree(hTextU); }
            }
            HGLOBAL hTextA = GlobalAlloc(GMEM_MOVEABLE, txt_len);
            if (hTextA) {
                char *p = (char *)GlobalLock(hTextA);
                if (p) {
                    memcpy(p, txt, txt_len);
                    GlobalUnlock(hTextA);
                    SetClipboardData(CF_TEXT, hTextA);
                } else { GlobalFree(hTextA); }
            }
        }
        log_msg("RECV index=%u - CF_TEXT/CF_UNICODETEXT set, hdrop=%s\n",
                (unsigned)req_index, hdrop_name(ct.hdrop));

        if (ct.hdrop == CLIPTEST_HDROP_YES) {
            /* CF_HDROP: point at the input file matching from_format.
               For DIB/DIBV5, create a temp file since no .dib inputs exist. */
            char drop_path[512];

            if (ct.from_format == CLIPTEST_FORMAT_DIB ||
                ct.from_format == CLIPTEST_FORMAT_DIBV5) {
                /* Build DIB from PNG, write to temp file */
                int w, h;
                unsigned char *pixels = stbi_load(png_path, &w, &h, NULL, 4);
                if (!pixels) {
                    log_msg("RECV index=%u - failed to load %s\n",
                            (unsigned)req_index, png_path);
                    CloseClipboard();
                    goto send_response;
                }
                int is_v5 = (ct.from_format == CLIPTEST_FORMAT_DIBV5
                         || ct.transparency == CLIPTEST_TRANSPARENCY_YES);
                size_t dib_len;
                unsigned char *dib = build_dib(w, h, pixels, ct.transparency,
                                                is_v5, &dib_len);
                stbi_image_free(pixels);
                if (!dib) {
                    CloseClipboard();
                    goto send_response;
                }

                const char *ext = (ct.from_format == CLIPTEST_FORMAT_DIBV5)
                                  ? "dibv5" : "dib";
                snprintf(drop_path, sizeof(drop_path),
                         "input\\dalmatian-%s-%s.%s",
                         size_name(ct.width),
                         (ct.transparency == CLIPTEST_TRANSPARENCY_YES)
                             ? "circle" : "square",
                         ext);
                FILE *fp = fopen(drop_path, "wb");
                if (fp) {
                    fwrite(dib, 1, dib_len, fp);
                    fclose(fp);
                }
                free(dib);
            } else {
                build_image_path(&ct, ct.from_format, drop_path, sizeof(drop_path));
            }

            /* Build DROPFILES + filename for CF_HDROP.
               Resolve to an absolute path so VcXsrv can open the file
               regardless of its working directory. */
            char abs_path[512];
            if (!GetFullPathNameA(drop_path, sizeof(abs_path), abs_path, NULL)) {
                log_msg("RECV index=%u - GetFullPathNameA failed for %s (err=%lu)\n",
                        (unsigned)req_index, drop_path, GetLastError());
                CloseClipboard();
                goto send_response;
            }
            int path_bytes = (int)strlen(abs_path) + 1;
            int df_size = sizeof(DROPFILES) + path_bytes + 1;
            HGLOBAL hDrop = GlobalAlloc(GMEM_MOVEABLE, df_size);
            if (hDrop) {
                DROPFILES *df = (DROPFILES *)GlobalLock(hDrop);
                if (df) {
                    memset(df, 0, df_size);
                    df->pFiles = sizeof(DROPFILES);
                    df->fWide = FALSE;
                    memcpy((char *)df + sizeof(DROPFILES), abs_path, path_bytes);
                    GlobalUnlock(hDrop);
                    SetClipboardData(CF_HDROP, hDrop);
                } else {
                    GlobalFree(hDrop);
                }
            }
        } else {
            /* hdrop=NO: put CF_DIB / CF_DIBV5 on the clipboard. */
            int w, h;
            unsigned char *pixels = stbi_load(png_path, &w, &h, NULL, 4);
            if (!pixels) {
                log_msg("RECV index=%u - stbi_load FAILED for %s (err=%s)\n",
                        (unsigned)req_index, png_path, stbi_failure_reason());
                CloseClipboard();
                goto send_response;
            }
            log_msg("RECV index=%u - loaded %dx%d RGBA from PNG\n",
                    (unsigned)req_index, w, h);

            int is_v5 = (ct.from_format == CLIPTEST_FORMAT_DIBV5
                         || ct.transparency == CLIPTEST_TRANSPARENCY_YES);
            size_t dib_len;
            unsigned char *dib = build_dib(w, h, pixels, ct.transparency,
                                            is_v5, &dib_len);
            stbi_image_free(pixels);
            if (!dib) {
                log_msg("RECV index=%u - build_dib FAILED\n", (unsigned)req_index);
                CloseClipboard();
                goto send_response;
            }
            log_msg("RECV index=%u - DIB built: %zu bytes, is_v5=%d\n",
                    (unsigned)req_index, dib_len, is_v5);

            HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dib_len);
            if (hDib) {
                void *dst = GlobalLock(hDib);
                if (dst) {
                    memcpy(dst, dib, dib_len);
                    GlobalUnlock(hDib);
                    HANDLE hRet;
                    if (is_v5)
                        hRet = SetClipboardData(CF_DIBV5, hDib);
                    else
                        hRet = SetClipboardData(CF_DIB, hDib);
                    if (hRet)
                        log_msg("RECV index=%u - SetClipboardData(%s) OK\n",
                                (unsigned)req_index, is_v5 ? "CF_DIBV5" : "CF_DIB");
                    else {
                        log_msg("RECV index=%u - SetClipboardData(%s) FAILED: %lu\n",
                                (unsigned)req_index, is_v5 ? "CF_DIBV5" : "CF_DIB",
                                GetLastError());
                        GlobalFree(hDib);
                    }
                } else {
                    log_msg("RECV index=%u - GlobalLock FAILED\n", (unsigned)req_index);
                    GlobalFree(hDib);
                }
            } else {
                log_msg("RECV index=%u - GlobalAlloc FAILED for %zu bytes\n",
                        (unsigned)req_index, dib_len);
            }
            free(dib);

            /* Also set registered formats -- TEMPORARILY DISABLED for debugging.
               VcXsrv hardcodes format numbers that may not match this system.
               We advertise at BOTH the VcXsrv hardcoded number AND the
               system-specific number from RegisterClipboardFormat. */
            if (0 && ct.from_format != CLIPTEST_FORMAT_DIB &&
                ct.from_format != CLIPTEST_FORMAT_DIBV5) {
                UINT vcx_fmt = 0;
                UINT sys_fmt = 0;
                if (ct.from_format == CLIPTEST_FORMAT_PNG) {
                    vcx_fmt = fmt_vcx_png;
                    sys_fmt = RegisterClipboardFormatA("PNG");
                } else if (ct.from_format == CLIPTEST_FORMAT_GIF) {
                    vcx_fmt = fmt_vcx_gif;
                    sys_fmt = RegisterClipboardFormatA("GIF");
                } else if (ct.from_format == CLIPTEST_FORMAT_JPEG) {
                    vcx_fmt = fmt_vcx_jfif;
                    sys_fmt = RegisterClipboardFormatA("JFIF");
                }

                char fmt_path[512];
                build_image_path(&ct, ct.from_format, fmt_path, sizeof(fmt_path));
                log_msg("RECV index=%u - loading reg fmt vcx=%u sys=%u from %s\n",
                        (unsigned)req_index, (unsigned)vcx_fmt, (unsigned)sys_fmt,
                        fmt_path);
                FILE *fp = fopen(fmt_path, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    long sz = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    if (sz > 0 && sz <= (long)MAX_IMAGE_SIZE) {
                        unsigned char *file_data = malloc((size_t)sz);
                        if (file_data && fread(file_data, 1, (size_t)sz, fp) == (size_t)sz) {
                            /* Set at VcXsrv hardcoded format number */
                            {
                                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)sz);
                                if (h) {
                                    void *d = GlobalLock(h);
                                    if (d) {
                                        memcpy(d, file_data, (size_t)sz);
                                        GlobalUnlock(h);
                                        HANDLE hr = SetClipboardData(vcx_fmt, h);
                                        if (hr)
                                            log_msg("RECV index=%u - SetClipboardData(vcx=%u) OK\n",
                                                    (unsigned)req_index, (unsigned)vcx_fmt);
                                        else {
                                            log_msg("RECV index=%u - SetClipboardData(vcx=%u) FAILED: %lu\n",
                                                    (unsigned)req_index, (unsigned)vcx_fmt,
                                                    GetLastError());
                                            GlobalFree(h);
                                        }
                                    } else { GlobalFree(h); }
                                }
                            }
                            /* Set at system-specific format number (if different) */
                            if (sys_fmt && sys_fmt != vcx_fmt) {
                                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)sz);
                                if (h) {
                                    void *d = GlobalLock(h);
                                    if (d) {
                                        memcpy(d, file_data, (size_t)sz);
                                        GlobalUnlock(h);
                                        HANDLE hr = SetClipboardData(sys_fmt, h);
                                        if (hr)
                                            log_msg("RECV index=%u - SetClipboardData(sys=%u) OK\n",
                                                    (unsigned)req_index, (unsigned)sys_fmt);
                                        else {
                                            log_msg("RECV index=%u - SetClipboardData(sys=%u) FAILED: %lu\n",
                                                    (unsigned)req_index, (unsigned)sys_fmt,
                                                    GetLastError());
                                            GlobalFree(h);
                                        }
                                    } else { GlobalFree(h); }
                                }
                            }
                            free(file_data);
                        } else {
                            if (file_data) free(file_data);
                            log_msg("RECV index=%u - reg fmt fread FAILED\n",
                                    (unsigned)req_index);
                        }
                    }
                    fclose(fp);
                } else {
                    log_msg("RECV index=%u - reg fmt fopen FAILED for %s\n",
                            (unsigned)req_index, fmt_path);
                }
            }
        }

        /* Diagnostic: enumerate formats on clipboard before close */
        {
            UINT efmt = 0;
            log_msg("RECV index=%u - enum: ", (unsigned)req_index);
            while ((efmt = EnumClipboardFormats(efmt)) != 0) {
                char name[128];
                if (GetClipboardFormatNameA(efmt, name, sizeof(name)))
                    log_msg(" fmt=%u(%s)", (unsigned)efmt, name);
                else {
                    const char *stdname = "?";
                    switch (efmt) {
                    case CF_TEXT:            stdname = "CF_TEXT"; break;
                    case CF_UNICODETEXT:     stdname = "CF_UNICODETEXT"; break;
                    case CF_DIB:             stdname = "CF_DIB"; break;
                    case CF_DIBV5:           stdname = "CF_DIBV5"; break;
                    case CF_HDROP:           stdname = "CF_HDROP"; break;
                    }
                    log_msg(" %s(%u)", stdname, (unsigned)efmt);
                }
            }
            log_msg("\n");
        }

        CloseClipboard();
        log_msg("RECV index=%u - clipboard set, sending ack\n", (unsigned)req_index);
        ret = 0;  /* success — ack only, no image in response */
    } else {
        /* X11_TO_WIN32: always passthrough — read DIB/DIBV5 from clipboard
           and send raw bytes back over the socket. */
        UINT cf = (ct.to_format == CLIPTEST_FORMAT_DIBV5) ? CF_DIBV5 : CF_DIB;

        if (!OpenClipboard(g_hwnd)) {
            log_msg("RECV index=%u - OpenClipboard for paste failed: %lu\n",
                    (unsigned)req_index, GetLastError());
            goto send_response;
        }

        HANDLE h = GetClipboardData(cf);
        if (!h) {
            log_msg("RECV index=%u - GetClipboardData failed: %lu\n",
                    (unsigned)req_index, GetLastError());
            CloseClipboard();
            goto send_response;
        }

        SIZE_T cb = GlobalSize(h);
        const unsigned char *src = (const unsigned char *)GlobalLock(h);
        if (!src || cb < 40) {
            if (src) GlobalUnlock(h);
            CloseClipboard();
            goto send_response;
        }

        image_out_len = cb;
        image_out = (unsigned char *)malloc(cb);
        if (image_out)
            memcpy(image_out, src, cb);
        GlobalUnlock(h);
        CloseClipboard();

        /* Track which CF was actually received for the ext field */
        memcpy(resp_ext, (cf == CF_DIBV5) ? "dibv5" : "dib", 8);

        log_msg("RECV index=%u - X11_TO_WIN32: got %s %zu bytes\n",
                (unsigned)req_index, resp_ext, image_out_len);
        ret = 0;
    }

send_response:
    {
        /* Build response: type + length + index + success + ext[8] + image data */
        uint32_t resp_hdr[4];
        resp_hdr[0] = MSG_RESPONSE;
        resp_hdr[1] = CLIPTEST_RESPONSE_BASE + (uint32_t)(image_out ? image_out_len : 0);
        resp_hdr[2] = req_index;
        resp_hdr[3] = (ret == 0) ? 1 : 0;  /* success */

        log_msg("SEND type=MSG_RESPONSE len=%u index=%u success=%u ext=%.8s imgsize=%zu\n",
                (unsigned)resp_hdr[1], (unsigned)resp_hdr[2],
                (unsigned)resp_hdr[3], resp_ext, image_out ? image_out_len : 0);

        if (send_all(client, resp_hdr, 16) < 0) {
            log_msg("SEND type=MSG_RESPONSE index=%u - header send failed\n",
                    (unsigned)req_index);
            ret = -1;
        } else if (send_all(client, resp_ext, 8) < 0) {
            log_msg("SEND type=MSG_RESPONSE index=%u - ext send failed\n",
                    (unsigned)req_index);
            ret = -1;
        } else if (image_out && image_out_len > 0) {
            if (send_all(client, image_out, (int)image_out_len) < 0) {
                log_msg("SEND type=MSG_RESPONSE index=%u - image send failed\n",
                        (unsigned)req_index);
                ret = -1;
            }
        }
    }

    free(payload);
    free(image_out);
    return ret;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    const char *listen_addr = "0.0.0.0";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-listen") == 0 && i + 1 < argc)
            listen_addr = argv[++i];
        else {
            fprintf(stderr, "Usage: %s [-listen <addr>]\n", argv[0]);
            return 1;
        }
    }

    /* Initialize Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_msg("WSAStartup failed\n");
        return 1;
    }

    /* Create hidden window for clipboard operations */
    {
        WNDCLASSEX wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = clip_wndproc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.lpszClassName = "win32cliptest_clip";
        if (RegisterClassEx(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            g_hwnd = CreateWindowEx(0, "win32cliptest_clip", "", WS_POPUP,
                                     0, 0, 1, 1, NULL, NULL,
                                     GetModuleHandle(NULL), NULL);
        }
        if (!g_hwnd)
            log_msg("WARNING: could not create clipboard window\n");
    }

    init_format_ids();

    /* Create listening socket */
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) {
        log_msg("socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&opt, sizeof(opt));

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(LISTEN_PORT);
    if (strcmp(listen_addr, "0.0.0.0") == 0)
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        addr.sin_addr.s_addr = inet_addr(listen_addr);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, 1) == SOCKET_ERROR) {
        log_msg("listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    log_msg("win32cliptest listening on %s:%d\n", listen_addr, LISTEN_PORT);

    /* Accept loop */
    int running = 1;
    while (running) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            log_msg("accept() failed: %d\n", WSAGetLastError());
            continue;
        }

        log_msg("Client connected from %s:%d\n",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        /* Handle requests on this connection until error/close/quit */
        int rc;
        while ((rc = handle_request(client)) == 0)
            ;
        if (rc == -2) running = 0;

        closesocket(client);
        log_msg("Client disconnected\n");
    }

    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
