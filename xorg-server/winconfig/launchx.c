#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winreg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bcrypt.h>

#include "libssh2.h"

#include "../Nuklear/nukleardefault.h"

#include "terminal.h"
#include "ssh.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <math.h>

#include "../../../password-key.c"

#define LIBSSH2_BCRYPT_PBKDF_C
#include "blowfish.c"

#include "x_logo_rgb.h"

#define IPS_STATIC static
#include "../../xorg-server/os/ipv6scan.c"
#undef IPS_STATIC

#define IDI_LAUNCHX 101

/* nuklear_gdi.h calls nk_cos/nk_sin, which are internal (static) nuklear
   helpers not exported by libnuklear.a; provide local wrappers. */
static float nk_cos(const float x) { return cosf(x); }
static float nk_sin(const float x) { return sinf(x); }

#define NK_GDI_IMPLEMENTATION
#include "../Nuklear/demo/gdi/nuklear_gdi.h"

int inaddr_validate(const char *s, int *family);

/* Build an HBITMAP-backed nk_image from the embedded RGB logo. The GDI
   backend draws whatever HBITMAP is in handle.ptr, so this is the only
   place image bytes touch the backend. */
static struct nk_image
launchx_make_logo_image(void)
{
    struct nk_image img;
    BITMAPINFO bi;
    HBITMAP hbm;
    unsigned char *bits;
    int stride, x, y;

    memset(&img, 0, sizeof(img));
    memset(&bi, 0, sizeof(bi));

    stride = ((X_LOGO_W * 3 + 3) & ~3);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = X_LOGO_W;
    bi.bmiHeader.biHeight = -X_LOGO_H;   /* top-down DIB */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = stride * X_LOGO_H;

    hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!hbm)
        return img;

    /* 24bpp DIBs store BGR, so swap the red/blue channels of the RGB source. */
    for (y = 0; y < X_LOGO_H; ++y) {
        const unsigned char *src = x_logo_rgb + y * X_LOGO_W * 3;
        unsigned char *dst = bits + y * stride;
        for (x = 0; x < X_LOGO_W; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }

    img.handle.ptr = hbm;
    img.w = X_LOGO_W;
    img.h = X_LOGO_H;
    img.region[0] = 0;
    img.region[1] = 0;
    img.region[2] = X_LOGO_W;
    img.region[3] = X_LOGO_H;
    return img;
}

static void
launchx_free_logo_image(struct nk_image *img)
{
    if (img && img->handle.ptr) {
        DeleteObject((HBITMAP)img->handle.ptr);
        img->handle.ptr = NULL;
    }
}

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600
#define TAB_WIDTH     260

static const char *tab_names[12] = {
    "SSH login",
    "Networking & access control", "XDMCP", "Screen & windowing modes",
    "Pointer & keyboard input", "XKB keyboard layout", "AccessX key sequences",
    "Windows desktop integration", "OpenGL / GLX",
    "Fonts, rendering & appearance", "Logging, scheduling & extensions",
    "Audio"
};

static const char *maxclients_items[] = {"64", "128", "256", "512", "1024", "2048"};

#define MAXCLIENTS_COUNT (sizeof(maxclients_items) / sizeof(maxclients_items[0]))

static const char *listeningport_items[] = {
    "Display :0 on tcp port 6000",
    "Display :1 on tcp port 6001",
    "Display :2 on tcp port 6002",
    "Display :3 on tcp port 6003",
    "Display :4 on tcp port 6004",
    "Display :5 on tcp port 6005",
    "Display :6 on tcp port 6006",
    "Display :7 on tcp port 6007",
    "Display :8 on tcp port 6008",
    "Display :9 on tcp port 6009",
    "Display :10 on tcp port 6010",
    "Display :11 on tcp port 6011",
    "Display :12 on tcp port 6012"
};
#define LISTENINGPORT_COUNT (sizeof(listeningport_items) / sizeof(listeningport_items[0]))

static const char *resize_items[] = {"none", "scrollbars", "randr"};
#define RESIZE_COUNT (sizeof(resize_items) / sizeof(resize_items[0]))

static const char *depth_items[] = {"Auto", "8", "15", "16", "24", "32"};
#define DEPTH_COUNT (sizeof(depth_items) / sizeof(depth_items[0]))

static const char *engine_items[] = {"Auto", "Shadow GDI (1)", "Shadow DirectDraw4 (4)"};
#define ENGINE_COUNT (sizeof(engine_items) / sizeof(engine_items[0]))

static const char *xinerama_items[] = {"Disabled", "Enabled"};
#define XINERAMA_COUNT (sizeof(xinerama_items) / sizeof(xinerama_items[0]))

static const char *render_items[] = {"default", "mono", "gray", "color"};
#define RENDER_COUNT (sizeof(render_items) / sizeof(render_items[0]))

static const char *deferglyphs_items[] = {"none", "all", "16"};
#define DEFERGLYPHS_COUNT (sizeof(deferglyphs_items) / sizeof(deferglyphs_items[0]))

static const char *backingstore_items[] = {"Default", "Enable (+bs)", "Disable (-bs)"};
#define BACKINGSTORE_COUNT (sizeof(backingstore_items) / sizeof(backingstore_items[0]))

#define NUM_EXTENSIONS 16
static const char *extension_names[NUM_EXTENSIONS] = {
    "SHAPE", "XTEST", "SECURITY", "XINERAMA", "XFIXES",
    "XFree86-Bigfont", "RENDER", "RANDR", "COMPOSITE", "DAMAGE",
    "MIT-SCREEN-SAVER", "DOUBLE-BUFFER", "RECORD", "DPMS",
    "X-Resource", "GLX"
};

static const char *audio_listen_items[] = {
    "Loopback only (127.0.0.1)",
    "All interfaces (0.0.0.0)",
    "All interfaces (IPv4 + IPv6)"
};
#define AUDIO_LISTEN_COUNT (sizeof(audio_listen_items) / sizeof(audio_listen_items[0]))

static const char *audio_auth_items[] = {
    "Cookie (shared pulse-cookie)",
    "Anonymous",
    "IP allow-list"
};
#define AUDIO_AUTH_COUNT (sizeof(audio_auth_items) / sizeof(audio_auth_items[0]))

static const char *audio_rate_items[] = {
    "Auto (44100 Hz)", "44100 Hz", "48000 Hz", "96000 Hz", "192000 Hz"
};
#define AUDIO_RATE_COUNT (sizeof(audio_rate_items) / sizeof(audio_rate_items[0]))

static const int audio_rate_values[] = { 0, 44100, 48000, 96000, 192000 };

static const char *audio_format_items[] = {
    "Auto (s16le)", "s16le (16-bit)", "s24le (24-bit)", "s32le (32-bit)"
};
#define AUDIO_FORMAT_COUNT (sizeof(audio_format_items) / sizeof(audio_format_items[0]))

static const char *audio_format_values[] = { "", "s16le", "s24le", "s32le" };

static const char *audio_channels_items[] = {
    "Auto (stereo)", "Mono (1)", "Stereo (2)"
};
#define AUDIO_CHANNELS_COUNT (sizeof(audio_channels_items) / sizeof(audio_channels_items[0]))

static const int audio_channels_values[] = { 0, 1, 2 };

static const char *audio_latency_items[] = {
    "Default", "Low", "Medium", "High"
};
#define AUDIO_LATENCY_COUNT (sizeof(audio_latency_items) / sizeof(audio_latency_items[0]))

static const int audio_latency_fragments[] = { 0, 3, 5, 8 };
static const int audio_latency_fragment_size[] = { 0, 4096, 8192, 16384 };

#define MAX_AUDIO_DEVICES 16
static char audio_output_devices[MAX_AUDIO_DEVICES][MAXPNAMELEN];
static int audio_output_device_count;
static char audio_input_devices[MAX_AUDIO_DEVICES][MAXPNAMELEN];
static int audio_input_device_count;

static ssh_session *g_ssh;
static int *g_current_tab;

static int g_focus_idx = -1;        /* focused widget in tab order, -1 = none */
static int g_focus_count = 0;       /* focusable widgets drawn last frame */
static int g_focus_seq = 0;         /* running counter while drawing this frame */
static int g_focus_on = 0;          /* focus nav active (SSH login tab, not connected) */
static int g_unfocus_edits = 0;     /* Tab pressed: clear edit focus this frame */
static int g_activate_pressed = 0;  /* Enter/Space pressed: activate focused button */
static int g_confirm_reset = 0;     /* Reset confirmation dialog is open (modal) */
static int g_confirm_exit = 0;      /* Exit-with-live-connections dialog is open (modal) */
static char g_notice[512];          /* cross-tab conflict explanation (modal) */
static char g_acl_error[512];       /* ACL validation error (modal) */
static char g_hyperv_error[1024];   /* Hyper-V registry write error (modal) */

static const void *g_field_menu_owner; /* unique key of the field whose context menu is open */
static int g_field_menu_copy;       /* menu shows Copy (frozen for the menu lifetime) */
static int g_field_menu_editable;   /* menu shows Paste (field is editable) */
static int g_field_menu_size;       /* size of the owned field buffer */
static int g_field_menu_seen;       /* owner field was rendered this frame */

/* PuTTY's Ctrl-key method: translate the keydown ourselves with the Ctrl
   state intact so Ctrl-C/D/etc. become the raw control byte (0x03, 0x04, ...)
   instead of being stolen as clipboard shortcuts or dropped (< 0x20). */
static int
translate_ctrl_key(WPARAM wParam, LPARAM lParam, char *out)
{
    BYTE keystate[256];
    int shift_state;
    char *p = out;

    if (HIWORD(lParam) & KF_UP)
        return 0;                          /* key-up */
    if (!GetKeyboardState(keystate))
        return 0;

    shift_state = ((keystate[VK_SHIFT] & 0x80) != 0)
                + ((keystate[VK_CONTROL] & 0x80) != 0) * 2;

    if (!(shift_state & 2))
        return 0;                          /* Ctrl not held */
    if (keystate[VK_MENU] & 0x80)
        return 0;                          /* AltGr: leave to normal translation */

    if (wParam == VK_SPACE && shift_state == 2) {          /* Ctrl-Space -> NUL */
        *p++ = 0x00;
        return (int)(p - out);
    }
    if (shift_state == 2 && wParam >= '2' && wParam <= '8') {  /* Ctrl-2..8 */
        *p++ = "\000\033\034\035\036\037\177"[wParam - '2'];
        return (int)(p - out);
    }
    if (shift_state == 2 && (wParam == 0xBD || wParam == 0xBF)) { /* Ctrl+- / Ctrl+/ */
        *p++ = 0x1F;
        return (int)(p - out);
    }
    if (shift_state == 2 && (wParam == 0xDF || wParam == 0xDC)) { /* Ctrl+_ / Ctrl+\ */
        *p++ = 0x1C;
        return (int)(p - out);
    }
    if (shift_state == 3 && wParam == 0xDE) {              /* Ctrl-~ */
        *p++ = 0x1E;
        return (int)(p - out);
    }

    /* fall through: let the layout translate with Ctrl held (Ctrl+A..Z -> 0x01..0x1A) */
    {
        HKL layout = GetKeyboardLayout(0);
        WCHAR uni[4];
        int r = ToUnicodeEx((UINT)wParam, (int)((lParam >> 16) & 0xFF),
                            keystate, uni, 4, 0, layout);
        int i;
        for (i = 0; i < r; ++i)
            if (uni[i] < 0x20 || uni[i] == 0x7F)
                *p++ = (char)uni[i];
    }
    return (int)(p - out);
}

static LRESULT CALLBACK
WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_CLOSE) {
        /* Route the title-bar [X] close through the same live-connection
           confirmation as the Exit button. */
        if (g_ssh && ssh_x11_open_count(g_ssh) > 0)
            g_confirm_exit = 1;
        else
            return DefWindowProcW(wnd, msg, wparam, lparam);
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (g_ssh && g_current_tab &&
            ssh_session_is_active(g_ssh) && *g_current_tab == 0 &&
            !ssh_hostkey_pending(g_ssh)) {
            if (wparam == VK_TAB && !(HIWORD(lparam) & KF_UP)) {
                BYTE keystate[256];
                if (GetKeyboardState(keystate)) {
                    int shift = (keystate[VK_SHIFT] & 0x80) != 0;
                    int ctrl  = (keystate[VK_CONTROL] & 0x80) != 0;
                    int alt   = (keystate[VK_MENU] & 0x80) != 0;
                    if (!ctrl && !alt) {
                        if (shift)          /* PuTTY: Shift+Tab -> backtab */
                            ssh_send(g_ssh, "\x1b[Z", 3);
                        else                /* PuTTY: Tab -> 0x09 */
                            ssh_send(g_ssh, "\t", 1);
                        return 0;
                    }
                }
            }
            {
                char buf[8];
                int n = translate_ctrl_key(wparam, lparam, buf);
                if (n > 0) {
                    ssh_send(g_ssh, buf, (size_t)n);
                    return 0;
                }
            }
        }
    }
    if (nk_gdi_handle_event(wnd, msg, wparam, lparam))
        return 0;
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

struct options_network_and_access_control {
    int ac_enabled;
    char allow_string[1024];
    char auth_file[1024];
    int byteswap_enabled;
    int maxclients_sel;
    int maxbigreqsize;
    int listeningport_sel;
    char vmid[256];
    char vsockport[16];
    int hyperv_registry_enabled;
};

static void
option_tooltip(struct nk_context *ctx, const struct nk_rect bounds, const char *text)
{
    static float timer = 0.0f;
    nk_do_tooltip_delay(ctx, text, bounds, &timer);
}

static int
focus_pick(struct nk_context *ctx, struct nk_rect *b)
{
    int focused = 0;
    if (b)
        *b = nk_widget_bounds(ctx);
    if (g_focus_on) {
        focused = (g_focus_seq == g_focus_idx);
        g_focus_seq++;
    }
    return focused;
}

static void
focus_ring(struct nk_context *ctx, const struct nk_rect b)
{
    nk_stroke_rect(nk_window_get_canvas(ctx),
        nk_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2),
        0, 2.0f, nk_rgb(70, 140, 240));
}

static int
button_option(struct nk_context *ctx, const char *label)
{
    struct nk_rect b;
    int focused = focus_pick(ctx, &b);
    int clicked = nk_button_label(ctx, label);
    if (focused) {
        focus_ring(ctx, b);
        if (g_activate_pressed)
            clicked = 1;
    }
    return clicked;
}

static int
dialog_button(struct nk_context *ctx, const char *label)
{
    struct nk_rect b = nk_widget_bounds(ctx);
    nk_button_label(ctx, label);
    return nk_input_has_mouse_click_in_button_rect(&ctx->input, NK_BUTTON_LEFT, b) &&
           nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT);
}

static void
ok_dialog(struct nk_context *ctx, HWND wnd, const char *title,
          char *message, float height, float msg_height)
{
    RECT rc;
    struct nk_rect pr;

    GetClientRect(wnd, &rc);
    pr = nk_rect((rc.right - 640.0f) / 2.0f,
                 (rc.bottom - 150.0f) / 2.0f, 640.0f, height);

    if (nk_begin(ctx, title, pr, NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 15, 1);
        nk_label_wrap(ctx, " ");
        nk_layout_row_dynamic(ctx, msg_height, 1);
        nk_label_wrap(ctx, message);
        nk_layout_row_dynamic(ctx, 34, 1);
        if (dialog_button(ctx, "OK"))
            message[0] = '\0';
    }
    nk_end(ctx);
}

static void
checkbox_option(struct nk_context *ctx, const char *label, int *value, const char *tooltip)
{
    struct nk_rect b;
    nk_layout_row_dynamic(ctx, 30, 1);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, label, value);
    option_tooltip(ctx, b, tooltip);
}

static char *
clipboard_get_utf8(void)
{
    HGLOBAL mem;
    LPCWSTR wstr;
    char *utf8 = NULL;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
        return NULL;
    mem = GetClipboardData(CF_UNICODETEXT);
    if (mem) {
        wstr = (LPCWSTR)GlobalLock(mem);
        if (wstr) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
            if (n > 1) {
                utf8 = (char*)malloc((size_t)n);
                if (utf8)
                    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, n, NULL, NULL);
            }
            GlobalUnlock(mem);
        }
    }
    CloseClipboard();
    return utf8;
}

static void
clipboard_set_utf8(const char *utf8, int byte_len)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, NULL, 0);
    HGLOBAL h;
    wchar_t *wstr;

    if (wlen <= 0 || !OpenClipboard(NULL))
        return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (h) {
        wstr = (wchar_t*)GlobalLock(h);
        if (wstr) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, byte_len, wstr, wlen);
            wstr[wlen] = L'\0';
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
            h = NULL;
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

/* Map a mouse position to a character index within the just-rendered edit
   field, without disturbing its cursor/selection.  Every configuration field
   here is single-line, so a single-row glyph walk is sufficient; this mirrors
   nuklear's internal locate logic, which is static and not exported. */
static int
edit_char_at(struct nk_context *ctx, struct nk_rect bounds, struct nk_vec2 mouse)
{
    const struct nk_text_edit *edit = &ctx->text_edit;
    const struct nk_style_edit *se = &ctx->style.edit;
    const struct nk_user_font *font = ctx->style.font;
    struct nk_rect area;
    float x;
    float prev_x;
    int n, i;

    area.x = bounds.x + se->padding.x + se->border;
    area.w = bounds.w - (2.0f * se->padding.x + 2 * se->border);

    x = (mouse.x - area.x) + edit->scrollbar.x;
    if (x <= 0.0f)
        return 0;

    n = nk_str_len_char(&edit->string);
    prev_x = 0.0f;
    for (i = 0; i < n; ++i) {
        nk_rune unicode = 0;
        int len = 0;
        const char *str = nk_str_at_const(&edit->string, i, &unicode, &len);
        float w = font->width(font->userdata, font->height, str, len);
        if (x < prev_x + w) {
            if (x < prev_x + w / 2.0f)
                return i;
            return i + 1;
        }
        prev_x += w;
    }
    return n;
}

/* Copy the field's selected text (editable fields) or its whole value
   (read-only display) to the Windows clipboard. */
static void
field_copy(const struct nk_context *ctx, char *buffer, int editable)
{
    int len = (int)strlen(buffer);
    int lo, hi;

    if (!editable) {
        if (len > 0)
            clipboard_set_utf8(buffer, len);
        return;
    }

    lo = ctx->text_edit.select_start;
    hi = ctx->text_edit.select_end;
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;
    if (hi > lo)
        clipboard_set_utf8(buffer + lo, hi - lo);
}

/* Insert the Windows clipboard into the field buffer at the cursor, replacing
   the current selection. Newlines are flattened for the single-line field. */
static void
field_paste(struct nk_context *ctx, char *buffer, int buffer_size)
{
    char *clip = clipboard_get_utf8();
    int len, clen, i, lo, hi, a, b, space;

    if (!clip)
        return;

    clen = (int)strlen(clip);
    for (i = 0; i < clen; ++i)
        if (clip[i] == '\r' || clip[i] == '\n')
            clip[i] = ' ';

    len = (int)strlen(buffer);
    a = ctx->text_edit.select_start;
    b = ctx->text_edit.select_end;
    lo = a < b ? a : b;
    hi = a < b ? b : a;
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;

    space = buffer_size - 1 - (len - (hi - lo));
    if (space < 0) space = 0;
    if (clen > space) clen = space;

    memmove(buffer + lo + clen, buffer + hi, (size_t)(len - hi) + 1);
    memcpy(buffer + lo, clip, (size_t)clen);

    free(clip);
}

/* Right-click context menu for a text field, mirroring the terminal's
   Copy/Paste menu. copy_ok/paste_ok select which items may appear. */
static void
field_context_menu(struct nk_context *ctx, struct nk_rect bounds,
    char *buffer, int buffer_size, int copy_ok, int paste_ok,
    const void *owner)
{
    struct nk_rect item;
    int nitems;

    if (g_field_menu_owner == NULL) {
        if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, bounds) ||
            (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT) &&
             nk_input_is_mouse_hovering_rect(&ctx->input, bounds))) {
            if (copy_ok && paste_ok) {
                int idx = edit_char_at(ctx, bounds, ctx->input.mouse.pos);
                int a = ctx->text_edit.select_start;
                int b = ctx->text_edit.select_end;
                int lo = a < b ? a : b;
                int hi = a < b ? b : a;
                g_field_menu_copy = (a != b && idx >= lo && idx < hi);
            } else {
                g_field_menu_copy = copy_ok;
            }
            g_field_menu_editable = paste_ok;
            g_field_menu_owner = owner;
            g_field_menu_size = buffer_size;
        }
    }

    if (g_field_menu_owner != owner)
        return;

    g_field_menu_seen = 1;

    nitems = (g_field_menu_copy ? 1 : 0) + (g_field_menu_editable ? 1 : 0);
    if (nk_contextual_begin(ctx, 0, nk_vec2(120, 24.0f * nitems + 14.0f), bounds)) {
        if (g_field_menu_copy) {
            nk_layout_row_dynamic(ctx, 24, 1);
            item = nk_widget_bounds(ctx);
            nk_button_label_styled(ctx, &ctx->style.contextual_button, "Copy");
            if ((nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT) ||
                 nk_input_is_mouse_released(&ctx->input, NK_BUTTON_RIGHT)) &&
                nk_input_is_mouse_hovering_rect(&ctx->input, item)) {
                field_copy(ctx, buffer, g_field_menu_editable);
                nk_contextual_close(ctx);
            }
        }
        if (g_field_menu_editable) {
            nk_layout_row_dynamic(ctx, 24, 1);
            item = nk_widget_bounds(ctx);
            nk_button_label_styled(ctx, &ctx->style.contextual_button, "Paste");
            if ((nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT) ||
                 nk_input_is_mouse_released(&ctx->input, NK_BUTTON_RIGHT)) &&
                nk_input_is_mouse_hovering_rect(&ctx->input, item)) {
                field_paste(ctx, buffer, g_field_menu_size);
                nk_contextual_close(ctx);
            }
        }
        nk_contextual_end(ctx);
    } else {
        g_field_menu_owner = NULL;
        g_field_menu_copy = 0;
        g_field_menu_editable = 0;
        g_field_menu_size = 0;
    }
}

static nk_flags
text_option_cell(struct nk_context *ctx, const char *label, char *buffer, const int buffer_size, const char *tooltip, nk_flags align)
{
    struct nk_rect b;
    int focused;
    nk_flags ret;
    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, align);
    option_tooltip(ctx, b, tooltip);
    focused = focus_pick(ctx, &b);
    if (focused)
        nk_edit_focus(ctx, NK_EDIT_FIELD);
    ret = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, buffer_size, nk_filter_default);
    if (ret & NK_EDIT_ACTIVATED)
        g_focus_idx = g_focus_seq - 1;
    if (focused)
        focus_ring(ctx, b);
    option_tooltip(ctx, b, tooltip);
    field_context_menu(ctx, b, buffer, buffer_size, 1, 1, buffer);
    return ret;
}

static nk_flags
text_option(struct nk_context *ctx, const char *label, char *buffer, const int buffer_size, const char *tooltip)
{
    nk_layout_row_dynamic(ctx, 30, 2);
    return text_option_cell(ctx, label, buffer, buffer_size, tooltip, NK_TEXT_LEFT);
}

static nk_bool
readonly_filter(const struct nk_text_edit *box, nk_rune unicode)
{
    (void)box;
    (void)unicode;
    return 0;
}

static void
readonly_option(struct nk_context *ctx, const char *label, const char *value, const char *tooltip)
{
    struct nk_rect b;
    struct nk_rect eb;
    static char buf[256];

    nk_layout_row_dynamic(ctx, 30, 2);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    option_tooltip(ctx, b, tooltip);
    snprintf(buf, sizeof(buf), "%s", value);
    eb = nk_widget_bounds(ctx);
    nk_edit_string_zero_terminated(ctx,
        NK_EDIT_SELECTABLE | NK_EDIT_CLIPBOARD | NK_EDIT_AUTO_SELECT,
        buf, (int)sizeof(buf), readonly_filter);
    option_tooltip(ctx, eb, tooltip);
    field_context_menu(ctx, eb, buf, (int)sizeof(buf), 1, 0, label);
}

/* Full-row read-only text field: no label, selectable but not editable.
   Used for the Hyper-V registry path, which mirrors the vsock port. */
static void
readonly_option_fullrow(struct nk_context *ctx, const char *value, const char *tooltip)
{
    struct nk_rect eb;
    static char buf[512];

    nk_layout_row_dynamic(ctx, 30, 1);
    eb = nk_widget_bounds(ctx);
    snprintf(buf, sizeof(buf), "%s", value);
    nk_edit_string_zero_terminated(ctx,
        NK_EDIT_SELECTABLE | NK_EDIT_CLIPBOARD | NK_EDIT_AUTO_SELECT,
        buf, (int)sizeof(buf), readonly_filter);
    option_tooltip(ctx, eb, tooltip);
    field_context_menu(ctx, eb, buf, (int)sizeof(buf), 1, 0, "hyperv_registry_path");
}

/* Compute the GuestCommunicationServices registry key name from the VSock
   port. The service GUID is HV_GUID_VSOCK_TEMPLATE with the port encoded in
   Data1 (the first eight hex digits). */
static void
hyperv_registry_path(const char *vsockport, char *out, size_t outsz)
{
    unsigned int port = (unsigned int)strtoul(vsockport, NULL, 10);

    snprintf(out, outsz,
        "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\%08X-FACB-11E6-BD58-64006A7986D3",
        port);
}

/* Create/update the GuestCommunicationServices registry key for the current
   VSock port so the Hyper-V host will route incoming guest connections to
   the listener.  Requires elevation (HKLM).  Returns 0 on success, else the
   Win32 error code. */
static int
hyperv_update_registry(const struct options_network_and_access_control *net)
{
    char full[512];
    char element[64];
    unsigned int port;
    const char *subkey;
    HKEY hk = NULL;
    LONG rc;
    DWORD disp;

    port = (unsigned int)strtoul(net->vsockport, NULL, 10);
    hyperv_registry_path(net->vsockport, full, sizeof(full));
    subkey = full + 5;                  /* skip the "HKLM\" prefix */

    snprintf(element, sizeof(element), "ming64x Hyper-V VSock port %u", port);

    rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, NULL, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hk, &disp);
    if (rc != ERROR_SUCCESS)
        return (int)rc;

    rc = RegSetValueExA(hk, "ElementName", 0, REG_SZ,
        (const BYTE *)element, (DWORD)(strlen(element) + 1));
    RegCloseKey(hk);
    return (rc == ERROR_SUCCESS) ? 0 : (int)rc;
}

/* Relaunch launchx.exe elevated (UAC) to write the Hyper-V registry key,
   then wait for it to finish.  Returns the helper's exit code (0 on success,
   else a Win32 error code). */
static int
hyperv_self_elevate_write(const char *vsockport)
{
    char self[MAX_PATH];
    char args[160];
    SHELLEXECUTEINFOA sei;
    DWORD exit_code = ERROR_ACCESS_DENIED;

    if (GetModuleFileNameA(NULL, self, sizeof(self)) == 0)
        return ERROR_FILE_NOT_FOUND;

    snprintf(args, sizeof(args), "--write-hyperv-reg \"%s\"", vsockport);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExA(&sei))
        return (int)GetLastError();

    WaitForSingleObject(sei.hProcess, INFINITE);
    if (!GetExitCodeProcess(sei.hProcess, &exit_code))
        exit_code = GetLastError();
    CloseHandle(sei.hProcess);
    return (int)exit_code;
}

/* Write the Hyper-V registry key, self-elevating via UAC when the direct
   write is denied.  Returns 0 on success, else a Win32 error code. */
static int
hyperv_registry_write_elevated(const char *vsockport)
{
    struct options_network_and_access_control net;
    int rc;

    memset(&net, 0, sizeof(net));
    snprintf(net.vsockport, sizeof(net.vsockport), "%s", vsockport);

    rc = hyperv_update_registry(&net);
    if (rc == 0 || rc != ERROR_ACCESS_DENIED)
        return rc;

    return hyperv_self_elevate_write(vsockport);
}

/* Write the Hyper-V service GUID to the registry (elevating if necessary),
   surfacing any failure via the shared error modal.  Returns 0 on success,
   else the Win32 error code.  Skips a rewrite of the same port so the Start
   handler does not prompt for UAC again after the checkbox already wrote it. */
static int
hyperv_registry_apply(const struct options_network_and_access_control *net)
{
    static char written_port[16];
    int rc;

    if (strcmp(written_port, net->vsockport) == 0)
        return 0;

    rc = hyperv_registry_write_elevated(net->vsockport);
    if (rc != 0) {
        char path[512];

        hyperv_registry_path(net->vsockport, path, sizeof(path));
        snprintf(g_hyperv_error, sizeof(g_hyperv_error),
            "Could not write the Hyper-V service GUID to the registry:\n"
            "%s\n\n"
            "Incoming Hyper-V connections will be rejected.\n"
            "Approve the administrator prompt, or create the key manually.\n"
            "(Registry error %d)",
            path, rc);
    } else {
        snprintf(written_port, sizeof(written_port), "%s", net->vsockport);
    }
    return rc;
}

static void
password_option_cell(struct nk_context *ctx, const char *label, char *password,
    char *mask, const int size, const char *tooltip)
{
    struct nk_rect b;
    int old_len, new_len, i, s;
    int focused;
    nk_flags ret;
    char rebuilt[128];

    /* A password loaded from launchx.cnf arrives with the mask still empty;
       fill it with asterisks so the field shows instead of appearing blank.
       After any edit the mask stays in lockstep with the password, so an
       empty mask with a non-empty password only occurs right after load. */
    if (mask[0] == '\0' && password[0] != '\0') {
        int plen = (int)strlen(password);
        if (plen > size - 1)
            plen = size - 1;
        for (i = 0; i < plen; ++i)
            mask[i] = '*';
        mask[plen] = '\0';
    }
    old_len = (int)strlen(mask);

    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    option_tooltip(ctx, b, tooltip);
    focused = focus_pick(ctx, &b);
    if (focused)
        nk_edit_focus(ctx, NK_EDIT_FIELD);
    ret = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, mask, size, nk_filter_default);
    if (ret & NK_EDIT_ACTIVATED)
        g_focus_idx = g_focus_seq - 1;
    if (focused)
        focus_ring(ctx, b);
    option_tooltip(ctx, b, tooltip);
    field_context_menu(ctx, b, mask, size, 0, 1, mask);

    new_len = (int)strlen(mask);
    if (new_len == old_len)
        return;

    /* Reconstruct the real password from the edited mask.  '*' chars are
       inherited (in order) from the old password; any other char was typed
       this frame.  A length increase means insertion (the typed char does not
       consume an old char); an equal/shorter length means replacement or
       deletion (each position consumes one old char). */
    s = 0;
    for (i = 0; i < new_len; ++i) {
        if (mask[i] != '*') {
            rebuilt[i] = mask[i];
            if (new_len <= old_len)
                s++;
        }
        else {
            rebuilt[i] = password[s++];
        }
    }
    rebuilt[new_len] = '\0';
    strcpy(password, rebuilt);

    for (i = 0; i < new_len; ++i)
        mask[i] = '*';
    mask[new_len] = '\0';
}

static void
combobox_option(struct nk_context *ctx, const char *label, const char *const *items, const int count, int *selected, const char *tooltip)
{
    struct nk_rect b;
    nk_layout_row_dynamic(ctx, 30, 2);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    option_tooltip(ctx, b, tooltip);
    b = nk_widget_bounds(ctx);
    nk_combobox(ctx, items, count, selected, 25, nk_vec2(nk_widget_width(ctx), 200));
    option_tooltip(ctx, b, tooltip);
}

static int
is_hex_color(const char *s)
{
    int i;

    if (!s || strlen(s) != 6)
        return 0;
    for (i = 0; i < 6; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'F') ||
              (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int
color_option(struct nk_context *ctx, const char *label, char *hex,
    const int hex_size, const char *tooltip)
{
    struct nk_rect b;
    struct nk_colorf cf;
    struct nk_color col;
    char tmp[12];
    char old[12];
    int changed;

    if (!is_hex_color(hex)) {
        strncpy(hex, "0F0F1E", hex_size - 1);
        hex[hex_size - 1] = '\0';
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, label, NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 120, 1);
    b = nk_widget_bounds(ctx);
    option_tooltip(ctx, b, tooltip);

    strncpy(old, hex, sizeof(old) - 1);
    old[sizeof(old) - 1] = '\0';

    col = nk_rgb_hex(hex);
    cf = nk_color_picker(ctx, nk_color_cf(col), NK_RGB);
    col = nk_rgb_cf(cf);
    nk_color_hex_rgb(tmp, col);

    changed = (strcmp(old, tmp) != 0);
    strncpy(hex, tmp, hex_size - 1);
    hex[hex_size - 1] = '\0';
    return changed;
}

static GdiFont *g_bold_font;

static void
heading(struct nk_context *ctx, const char *text)
{
    nk_style_push_font(ctx, &g_bold_font->nk);
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, text, NK_TEXT_CENTERED);
    nk_style_pop_font(ctx);
}

struct options_audio {
    int audio_enabled;
    char pulseport[16];
    int mic_enabled;
    int speaker_enabled;

    int listen_sel;          /* 0 = loopback, 1 = all IPv4, 2 = IPv4 + IPv6 */
    int auth_sel;            /* 0 = cookie, 1 = anonymous, 2 = IP allow-list */
    char auth_acl[256];      /* semicolon-separated IPs for auth_sel == 2 */

    char output_device[128]; /* "" = system default (WAVE_MAPPER) */
    char input_device[128];

    int rate_sel;            /* 0 = auto, else index into audio_rate_values */
    int format_sel;          /* 0 = auto, else index into audio_format_values */
    int channels_sel;        /* 0 = auto, else index into audio_channels_values */
    int latency_sel;         /* 0 = default, else fragments/fragment_size preset */

    int null_sink_enabled;
    int loopback_enabled;
};

static void ensure_pulse_cookie(void);
static int read_pulse_cookie(unsigned char out[256]);
static int config_dir(char *out, const size_t outsz);

struct options_ssh_login {
    char host[128];
    char port[16];
    char username[128];
    char password[128];
    char password_mask[128];
    int x11_forwarding;
    int save_password;
};

static void
tab_ssh_login(struct nk_context *ctx, struct options_ssh_login *opt,
    terminal *term, ssh_session *ssh,
    struct options_network_and_access_control *net,
    struct options_audio *audio)
{
    int cols_before = term->ncols;
    int rows_before = term->nrows;
    struct nk_rect b;

    heading(ctx, "SSH login");

    nk_layout_row_dynamic(ctx, 30, 4);
    text_option_cell(ctx, "SSH Connect IP", opt->host, sizeof(opt->host),
        "Hostname or IP address of the SSH server to connect to", NK_TEXT_LEFT);
    text_option_cell(ctx, "SSH Connect port", opt->port, sizeof(opt->port),
        "TCP port of the SSH server (default 22)", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 30, 4);
    text_option_cell(ctx, "Login username", opt->username, sizeof(opt->username),
        "Username to authenticate with on the SSH server", NK_TEXT_LEFT);
    password_option_cell(ctx, "Login password", opt->password, opt->password_mask,
        sizeof(opt->password),
        "Password to authenticate with on the SSH server");

    nk_layout_row_dynamic(ctx, 30, 2);
    if (ssh_session_is_active(ssh))
        nk_widget_disable_begin(ctx);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, "X11 forwarding", &opt->x11_forwarding);
    option_tooltip(ctx, b, "Enable X11 forwarding over the SSH connection so X11 clients on the\nremote host can connect back to this X server");
    if (ssh_session_is_active(ssh))
        nk_widget_disable_end(ctx);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, "Save password", &opt->save_password);
    option_tooltip(ctx, b, "Save the login password (encrypted) in launchx.cnf.\nWhen off, the password is not written to the config file.");

    nk_layout_row_dynamic(ctx, 30, 1);
    if (ssh->display_error_pending) {
        nk_style_push_color(ctx, &ctx->style.text.color, nk_rgb(255, 0, 0));
        nk_label(ctx, ssh->display_error, NK_TEXT_LEFT);
        nk_style_pop_color(ctx);
    } else {
        nk_label(ctx, "", NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(ctx, 30, 1);
    if (ssh_session_is_active(ssh)) {
        if (button_option(ctx, "Disconnect"))
            ssh_session_stop(ssh);
    }
    else if (button_option(ctx, "Connect")) {
        unsigned char cookie[256];
        int have_cookie;

        if (inaddr_validate(opt->host, NULL)) {
            snprintf(ssh->display_error, sizeof ssh->display_error,
                     "Invalid IP address \"%s\"", opt->host);
            InterlockedExchange(&ssh->display_error_pending, 1);
        } else {
            int port = atoi(opt->port);
            if (port < 1 || port > 65535)
                port = 22;

            have_cookie = audio->audio_enabled && read_pulse_cookie(cookie);

            ssh_session_start(ssh, opt->host, opt->username, opt->password,
                port, net->listeningport_sel, opt->x11_forwarding,
                have_cookie, atoi(audio->pulseport),
                have_cookie ? cookie : NULL);
            ssh_request_resize(ssh, term->ncols, term->nrows);
        }
    }

    terminal_draw(ctx, term);
    if (!ssh_hostkey_pending(ssh) && !g_confirm_reset) {
        terminal_mouse(ctx, term);
        if (terminal_menu(ctx, term))
            ssh_paste_clipboard(ssh);
    }

    if (term->ncols != cols_before || term->nrows != rows_before)
        ssh_request_resize(ssh, term->ncols, term->nrows);
}

static void
tab_network_and_access_control(struct nk_context *ctx,
    struct options_network_and_access_control *opt)
{
    struct nk_rect b;
    nk_flags vsockport_flags;
    int port_committed;
    static char prev_vsockport[16];
    static int prev_hyperv_enabled;
    static int hyperv_seen;

    heading(ctx, "Networking & access control");

    combobox_option(ctx, "Listening port", listeningport_items, (int)LISTENINGPORT_COUNT, &opt->listeningport_sel, "Display number the server runs as. Clients connect on TCP port\n6000 plus this number (default :0).");

    checkbox_option(ctx, "Disable access control (-ac)", &opt->ac_enabled, "Disable host-based access control, so any host may connect and\nchange the access list. Use with caution.");

    if (opt->ac_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Allowed IP addresses (-allow)", opt->allow_string, (int)sizeof(opt->allow_string), "allow connections whose address matches a ';'-separated IP list\n(e.g. 192.168.1.0/24;10.0.0.5-10.0.0.9;FE80::1). ',' is also accepted.");
    if (opt->ac_enabled)
        nk_widget_disable_end(ctx);

    text_option(ctx, "Authorization file (-auth)", opt->auth_file, (int)sizeof(opt->auth_file), "File of authorization records used to authenticate client access.");

    checkbox_option(ctx, "Allow different-endianness clients (+byteswappedclients)", &opt->byteswap_enabled, "Allow connections from clients with a different byte order\n(endianness) than the server.");

    combobox_option(ctx, "Max clients (-maxclients)", maxclients_items, (int)MAXCLIENTS_COUNT, &opt->maxclients_sel, "Maximum number of clients allowed to connect (a power of two).");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Max bigreq size (MB)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Largest request the server will accept, in megabytes.");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 1, &opt->maxbigreqsize, 127, 1);
    option_tooltip(ctx, b, "Largest request the server will accept, in megabytes.");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->maxbigreqsize);

    text_option(ctx, "Hyper-V VM GUID (-vmid)", opt->vmid, (int)sizeof(opt->vmid),
        "Hyper-V virtual machine GUID to accept VSock connections from.");

    vsockport_flags = text_option(ctx, "Hyper-V VSock listen port (-vsockport)", opt->vsockport, (int)sizeof(opt->vsockport),
        "Port number to listen on for VSock connections. Default 106000.");

    {
        char path[512];
        hyperv_registry_path(opt->vsockport, path, sizeof(path));
        readonly_option_fullrow(ctx, path,
            "Full registry key under which the Hyper-V service GUID must be\nregistered to accept incoming Hyper-V connections.");
    }

    checkbox_option(ctx, "Update registry with Hyper-V GUID", &opt->hyperv_registry_enabled,
        "You need to check this to allow incoming Hyper-V connections");

    /* Write the registry entry when the box is checked, or when the vsockport
       edit commits (focus leaves the field), rather than on every keystroke. */
    if (!hyperv_seen) {
        snprintf(prev_vsockport, sizeof(prev_vsockport), "%s", opt->vsockport);
        prev_hyperv_enabled = opt->hyperv_registry_enabled;
        hyperv_seen = 1;
    } else {
        port_committed = (vsockport_flags & NK_EDIT_DEACTIVATED) &&
                         strcmp(prev_vsockport, opt->vsockport) != 0;
        if (port_committed)
            snprintf(prev_vsockport, sizeof(prev_vsockport), "%s", opt->vsockport);

        if (opt->hyperv_registry_enabled &&
            (prev_hyperv_enabled != opt->hyperv_registry_enabled || port_committed))
            hyperv_registry_apply(opt);

        prev_hyperv_enabled = opt->hyperv_registry_enabled;
    }
}

struct options_xdmcp {
    int xdmcp_enabled;
    char query_host[256];
    int broadcast_enabled;
    char indirect_host[256];
    int multicast_enabled;
    char port_string[16];
    char from_address[256];
    int once_enabled;
    char display_class[256];
    char cookie[256];
    char display_id[256];
};

static void
tab_xdmcp(struct nk_context *ctx, struct options_xdmcp *opt)
{
    heading(ctx, "XDMCP");

    checkbox_option(ctx, "Enable XDMCP", &opt->xdmcp_enabled,
        "Enable the XDMCP protocol so the server contacts a display manager.\nAll XDMCP settings below are disabled while this is off.");

    if (!opt->xdmcp_enabled)
        nk_widget_disable_begin(ctx);

    if (opt->xdmcp_enabled && opt->broadcast_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Query host (-query)", opt->query_host, (int)sizeof(opt->query_host), "Enable XDMCP and send Query packets to this host.");
    if (opt->xdmcp_enabled && opt->broadcast_enabled)
        nk_widget_disable_end(ctx);

    if (opt->xdmcp_enabled && !opt->broadcast_enabled && opt->query_host[0])
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Broadcast for XDMCP (-broadcast)", &opt->broadcast_enabled, "Enable XDMCP and broadcast a query to the network. The first\ndisplay manager to answer hosts the session.");
    if (opt->xdmcp_enabled && !opt->broadcast_enabled && opt->query_host[0])
        nk_widget_disable_end(ctx);

    text_option(ctx, "Indirect host (-indirect)", opt->indirect_host, (int)sizeof(opt->indirect_host), "Enable XDMCP and send IndirectQuery packets to this host.");

    checkbox_option(ctx, "IPv6 multicast (-multicast)", &opt->multicast_enabled, "Enable XDMCP and multicast a query to the network (IPv6 multicast).");

    text_option(ctx, "UDP port (-port)", opt->port_string, (int)sizeof(opt->port_string), "UDP port used for XDMCP packets (default 177).");

    text_option(ctx, "Local address (-from)", opt->from_address, (int)sizeof(opt->from_address), "Local address to connect from, useful when the machine has several\nnetwork interfaces.");

    checkbox_option(ctx, "Terminate after one session (-once)", &opt->once_enabled, "Exit the server when the XDMCP session ends, instead of resetting.");

    text_option(ctx, "Display class (-class)", opt->display_class, (int)sizeof(opt->display_class), "XDMCP display qualifier used when looking up display-specific\noptions (default MIT-unspecified).");

    text_option(ctx, "Magic cookie (-cookie)", opt->cookie, (int)sizeof(opt->cookie), "Private key shared with the display manager for XDM-AUTHORIZATION-1.");

    text_option(ctx, "Display ID (-displayID)", opt->display_id, (int)sizeof(opt->display_id), "Identifier the display manager uses to locate this display's\nshared key.");

    if (!opt->xdmcp_enabled)
        nk_widget_disable_end(ctx);
}

struct screen_geometry_entry {
    char width[16];
    char height[16];
    char x[16];
    char y[16];
    char monitor[16];
};

struct options_screen_windowing {
    struct screen_geometry_entry screens[3];
    int fullscreen_enabled;
    int rootless_enabled;
    int multiwindow_enabled;
    int nodecoration_enabled;
    int multimonitors_enabled;
    int resize_sel;
    int depth_sel;
    char refresh[16];
    int engine_sel;
    char dpi[16];
    int xinerama_sel;
    int disablexinerama_enabled;
    int lesspointer_enabled;
    int swcursor_enabled;
    int nocursor_enabled;
};

static int
screen_geometry_set(const struct options_screen_windowing *opt)
{
    int i;

    for (i = 0; i < 3; ++i)
        if (opt->screens[i].width[0] || opt->screens[i].height[0] ||
            opt->screens[i].x[0] || opt->screens[i].y[0] ||
            opt->screens[i].monitor[0])
            return 1;
    return 0;
}

static char *
screen_geometry_field(struct options_screen_windowing *opt, int s, int f)
{
    struct screen_geometry_entry *e = &opt->screens[s];

    switch (f) {
    case 0: return e->width;
    case 1: return e->height;
    case 2: return e->x;
    case 3: return e->y;
    default: return e->monitor;
    }
}

static const char *scr_geom_screen_label[3] = {
    "Screen 1", "Screen 2", "Screen 3",
};

static const char *scr_geom_field_label[5] = {
    "Width", "Height", "X-offset", "Y-offset", "Monitor",
};

static const char *scr_geom_tip[5] = {
    "Screen width in pixels. Leave empty to derive it from the height (3:2).",
    "Screen height in pixels. Leave empty to derive it from the width (3:2).",
    "Horizontal position of the screen within the virtual desktop.",
    "Vertical position of the screen within the virtual desktop.",
    "Windows monitor (1-based) to place this screen on; empty = default.",
};

static void
tab_screen_and_windowing(struct nk_context *ctx,
    struct options_screen_windowing *opt)
{
    int fs, rl, mw, nd, lp, resize_scrollbars, scr;
    int s;

    heading(ctx, "Screen & windowing modes");

    fs = opt->fullscreen_enabled;
    rl = opt->rootless_enabled;
    mw = opt->multiwindow_enabled;
    nd = opt->nodecoration_enabled;
    lp = opt->lesspointer_enabled;
    resize_scrollbars = (opt->resize_sel == 1);
    scr = screen_geometry_set(opt);

    if (mw || fs || rl || opt->multimonitors_enabled)
        nk_widget_disable_begin(ctx);
    for (s = 0; s < 3; ++s) {
        nk_style_push_font(ctx, &g_bold_font->nk);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, scr_geom_screen_label[s], NK_TEXT_LEFT);
        nk_style_pop_font(ctx);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[0], screen_geometry_field(opt, s, 0), 16, scr_geom_tip[0], NK_TEXT_RIGHT);
        text_option_cell(ctx, scr_geom_field_label[1], screen_geometry_field(opt, s, 1), 16, scr_geom_tip[1], NK_TEXT_RIGHT);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[2], screen_geometry_field(opt, s, 2), 16, scr_geom_tip[2], NK_TEXT_RIGHT);
        text_option_cell(ctx, scr_geom_field_label[3], screen_geometry_field(opt, s, 3), 16, scr_geom_tip[3], NK_TEXT_RIGHT);

        nk_layout_row_dynamic(ctx, 30, 4);
        text_option_cell(ctx, scr_geom_field_label[4], screen_geometry_field(opt, s, 4), 16, scr_geom_tip[4], NK_TEXT_RIGHT);
    }
    if (mw || fs || rl || opt->multimonitors_enabled)
        nk_widget_disable_end(ctx);

    if (mw || rl || nd || lp || resize_scrollbars || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Run in fullscreen mode (-fullscreen)", &opt->fullscreen_enabled, "Make the X server window fill the entire Windows desktop.");
    if (mw || rl || nd || lp || resize_scrollbars || scr)
        nk_widget_disable_end(ctx);

    if (mw || fs || nd || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Transparent root window (-rootless)", &opt->rootless_enabled, "Run rootless: the root window is hidden and only top-level X windows\nshow. Needs an external window manager; not with -multiwindow or\n-fullscreen.");
    if (mw || fs || nd || scr)
        nk_widget_disable_end(ctx);

    if (rl || fs || nd || scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Run in multiwindow mode (-multiwindow)", &opt->multiwindow_enabled, "Run multiwindow: each top-level X window becomes its own Windows\nwindow with a built-in window manager. Not with -rootless or\n-fullscreen.");
    if (rl || fs || nd || scr)
        nk_widget_disable_end(ctx);

    if (mw || rl || fs)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "No window border/titlebar (-nodecoration)", &opt->nodecoration_enabled, "Show the X window with no Windows border or title bar. Ignored when\n-fullscreen is set.");
    if (mw || rl || fs)
        nk_widget_disable_end(ctx);

    if (scr)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Use entire virtual screen (-multimonitors)", &opt->multimonitors_enabled, "Create one screen covering all monitors, with fake XINERAMA data\ndescribing each monitor.");
    if (scr)
        nk_widget_disable_end(ctx);

    if (fs)
        nk_widget_disable_begin(ctx);
    combobox_option(ctx, "Resize mode (-resize)", resize_items, RESIZE_COUNT, &opt->resize_sel, "How the X screen resizes: scrollbars adds window scrollbars, randr\nuses the RANDR extension. Default is randr.");
    if (fs)
        nk_widget_disable_end(ctx);

    if (!fs)
        nk_widget_disable_begin(ctx);
    combobox_option(ctx, "Bit depth (-depth)", depth_items, DEPTH_COUNT, &opt->depth_sel, "Color depth in bits per pixel for fullscreen mode with a DirectDraw\nengine. Ignored without -fullscreen.");
    if (!fs)
        nk_widget_disable_end(ctx);

    if (!fs)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Refresh rate (-refresh)", opt->refresh, (int)sizeof(opt->refresh), "Refresh rate (Hz) for fullscreen mode with a DirectDraw engine.\nIgnored without -fullscreen.");
    if (!fs)
        nk_widget_disable_end(ctx);

    combobox_option(ctx, "Engine (-engine)", engine_items, ENGINE_COUNT, &opt->engine_sel, "Override the automatically selected drawing engine: 1 = Shadow GDI,\n4 = Shadow DirectDraw4 Non-Locking.");

    text_option(ctx, "Screen resolution DPI (-dpi)", opt->dpi, (int)sizeof(opt->dpi), "Screen resolution in dots per inch, for all screens.");

    combobox_option(ctx, "XINERAMA (+xinerama/-xinerama)", xinerama_items, XINERAMA_COUNT, &opt->xinerama_sel, "Enable (+) or disable (-) the XINERAMA extension.");

    checkbox_option(ctx, "Disable XINERAMA extension (-disablexineramaextension)", &opt->disablexinerama_enabled, "Disable the XINERAMA extension.");

    if (fs)
        nk_widget_disable_begin(ctx);
    checkbox_option(ctx, "Hide Windows pointer (-lesspointer)", &opt->lesspointer_enabled, "Also hide the Windows pointer over inactive X windows, preventing a\nghost cursor. Only applies with -swcursor.");
    if (fs)
        nk_widget_disable_end(ctx);

    checkbox_option(ctx, "X11 software cursor (-swcursor)", &opt->swcursor_enabled, "Use the X11 software cursor instead of the Windows cursor.");

    checkbox_option(ctx, "Disable cursor (-nocursor)", &opt->nocursor_enabled, "Disable the X cursor so it is not drawn on any screen.");
}

struct options_pointer_keyboard {
    int emulate3buttons_enabled;
    char emulate3buttons_timeout[16];
    int winkill_enabled;
    int unixkill_enabled;
    int keyhook_enabled;
    int ignoreinput_enabled;
    int autorepeat_enabled;
    char pointer_acceleration[16];
    char pointer_threshold[16];
    int bell;
    char autorepeat_delay[16];
    char autorepeat_interval[16];
};

static void
tab_pointer_keyboard(struct nk_context *ctx, struct options_pointer_keyboard *opt)
{
    struct nk_rect b;

    heading(ctx, "Pointer & keyboard input");

    checkbox_option(ctx, "Emulate 3-button mouse (-emulate3buttons)", &opt->emulate3buttons_enabled, "Emulate a middle button when both buttons are pressed together.");

    text_option(ctx, "Emulate timeout (ms)", opt->emulate3buttons_timeout, (int)sizeof(opt->emulate3buttons_timeout), "Timeout (ms) within which pressing both buttons counts as a\nmiddle click.");

    checkbox_option(ctx, "Alt+F4 exits server (-winkill)", &opt->winkill_enabled, "Allow Alt+F4 to exit the X server.");

    checkbox_option(ctx, "Ctrl+Alt+Backspace exits server (-unixkill)", &opt->unixkill_enabled, "Allow Ctrl+Alt+Backspace to exit the X server.");

    checkbox_option(ctx, "Grab special Windows keys (-keyhook)", &opt->keyhook_enabled, "Install a low-level keyboard hook to send keys such as Alt+Tab and the\nMenu key to the X server.");

    checkbox_option(ctx, "Ignore keyboard and mouse input (-ignoreinput)", &opt->ignoreinput_enabled, "Ignore all keyboard and mouse input (for testing and debugging).");

    checkbox_option(ctx, "Enable auto-repeat (r)", &opt->autorepeat_enabled, "Enable (r) or disable (-r) key auto-repeat.");

    text_option(ctx, "Pointer acceleration (-a)", opt->pointer_acceleration, (int)sizeof(opt->pointer_acceleration), "Pointer acceleration: how much the pointer reports relative to how\nfar it actually moved.");

    text_option(ctx, "Pointer threshold (-t)", opt->pointer_threshold, (int)sizeof(opt->pointer_threshold), "Pointer threshold in pixels: how far the pointer must move before\nacceleration takes effect.");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Bell base (-f)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Bell (beep) volume, from 0 to 100.");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 0, &opt->bell, 100, 1);
    option_tooltip(ctx, b, "Bell (beep) volume, from 0 to 100.");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->bell);

    text_option(ctx, "Auto-repeat delay (-ardelay)", opt->autorepeat_delay, (int)sizeof(opt->autorepeat_delay), "Autorepeat delay in milliseconds: how long a key must be held before\nit starts repeating.");

    text_option(ctx, "Auto-repeat interval (-arinterval)", opt->autorepeat_interval, (int)sizeof(opt->autorepeat_interval), "Autorepeat interval in milliseconds: time between repeated keystrokes.");
}

struct options_xkb {
    char xkblayout[128];
    char xkbmodel[128];
    char xkbvariant[128];
    char xkboptions[128];
    char xkbrules[128];
    char xkbdir[256];
};

static void
tab_xkb(struct nk_context *ctx, struct options_xkb *opt)
{
    heading(ctx, "XKB keyboard layout");

    text_option(ctx, "Layout (-xkblayout)", opt->xkblayout, (int)sizeof(opt->xkblayout), "Keyboard layout to load at startup (e.g. de, us). Defaults to a layout\nmatching your current Windows layout.");

    text_option(ctx, "Model (-xkbmodel)", opt->xkbmodel, (int)sizeof(opt->xkbmodel), "Keyboard model to load (default pc105).");

    text_option(ctx, "Variant (-xkbvariant)", opt->xkbvariant, (int)sizeof(opt->xkbvariant), "Keyboard layout variant (e.g. nodeadkeys). Defaults to unset.");

    text_option(ctx, "Options (-xkboptions)", opt->xkboptions, (int)sizeof(opt->xkboptions), "XKB options to load at startup.");

    text_option(ctx, "Rules (-xkbrules)", opt->xkbrules, (int)sizeof(opt->xkbrules), "XKB rules file to use (default xorg).");

    text_option(ctx, "Base directory (-xkbdir)", opt->xkbdir, (int)sizeof(opt->xkbdir), "Base directory for XKB keyboard layout files.");
}

#define NUM_AX_CTRL 9
static const char *ax_ctrl_names[NUM_AX_CTRL] = {
    "Repeat keys", "Slow keys", "Bounce keys", "Sticky keys", "Mouse keys",
    "Mouse keys acceleration", "AccessX keys", "AccessX timeout", "AccessX feedback"
};
static const unsigned int ax_ctrl_bits[NUM_AX_CTRL] = {
    0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080, 0x100
};
static const char *ax_ctrl_keys[NUM_AX_CTRL] = {
    "accessx.xkbrepeatkeysmask", "accessx.xkbslowkeysmask",
    "accessx.xkbbouncekeysmask", "accessx.xkbstickykeysmask",
    "accessx.xkbmousekeysmask", "accessx.xkbmousekeysaccelmask",
    "accessx.xkbaccessxkeysmask", "accessx.xkbaccessxtimeoutmask",
    "accessx.xkbaccessxfeedbackmask"
};

static const char *ax_ctrl_tips[NUM_AX_CTRL] = {
    "Hold a key down to make it repeat.",
    "Require a key to be held briefly before it registers.",
    "Ignore rapid repeated presses of the same key.",
    "Modifiers stay active for the next key press (one-finger chords).",
    "Control the pointer using the numeric keypad.",
    "Speed up pointer movement while Mouse Keys is active.",
    "Keys that turn AccessX features on and off.",
    "Automatically turn off AccessX features after a delay.",
    "Give audible or visual feedback for AccessX actions."
};

#define NUM_AX_OPT 12
static const char *ax_opt_names[NUM_AX_OPT] = {
    "StickyKeys press feedback", "StickyKeys accept feedback",
    "Feature on/off feedback", "SlowKeys warning feedback",
    "Indicator feedback", "StickyKeys feedback",
    "Two keys (disable StickyKeys)", "Latch to lock",
    "StickyKeys release feedback", "StickyKeys reject feedback",
    "BounceKeys reject feedback", "Dumb bell feedback"
};
static const unsigned int ax_opt_bits[NUM_AX_OPT] = {
    0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080,
    0x100, 0x200, 0x400, 0x800
};
static const char *ax_opt_keys[NUM_AX_OPT] = {
    "accessx.xkbaxskpressfbmask", "accessx.xkbaxskacceptfbmask",
    "accessx.xkbaxfeaturefbmask", "accessx.xkbaxslowwarnfbmask",
    "accessx.xkbaxindicatorfbmask", "accessx.xkbaxstickykeysfbmask",
    "accessx.xkbaxtwokeysmask", "accessx.xkbaxlatchtolockmask",
    "accessx.xkbaxskreleasefbmask", "accessx.xkbaxskrejectfbmask",
    "accessx.xkbaxbkrejectfbmask", "accessx.xkbaxdumbbellfbmask"
};

static const char *ax_opt_tips[NUM_AX_OPT] = {
    "Beep when a modifier is pressed in Sticky Keys.",
    "Beep when a Sticky Keys chord is completed.",
    "Beep when an AccessX feature is turned on or off.",
    "Beep when Slow Keys rejects a key press.",
    "Show feedback using the keyboard indicators.",
    "Use the full Sticky Keys feedback scheme.",
    "Press two keys at once to turn Sticky Keys off.",
    "Pressing a modifier twice locks it on.",
    "Beep when a Sticky Keys chord is released.",
    "Beep when Sticky Keys rejects a key press.",
    "Beep when Bounce Keys rejects a key press.",
    "Use a simple bell for feedback instead of distinct tones."
};

struct options_accessx {
    int enabled;
    char timeout[16];
    int timeout_mask[NUM_AX_CTRL];
    int feedback;
    int options_mask[NUM_AX_OPT];
};

/* Soft-dim widgets that are not currently emitted: replicate Nuklear's
   disabled look (color_factor = 0.5) without actually blocking input, so a
   trailing parameter can still be edited and thereby "light up". */
static void
dim_push(struct nk_context *ctx)
{
    nk_style_push_float(ctx, &ctx->style.text.color_factor, 0.5f);
    nk_style_push_float(ctx, &ctx->style.checkbox.color_factor, 0.5f);
    nk_style_push_float(ctx, &ctx->style.edit.color_factor, 0.5f);
}

static void
dim_pop(struct nk_context *ctx)
{
    nk_style_pop_float(ctx);
    nk_style_pop_float(ctx);
    nk_style_pop_float(ctx);
}

static void
tab_accessx(struct nk_context *ctx, struct options_accessx *opt)
{
    int i, timeout, want_t, want_m, want_f, want_o;
    int sent_t, sent_m, sent_f, sent_o;
    unsigned int tmask = 0, omask = 0;

    heading(ctx, "AccessX key sequences");

    timeout = opt->timeout[0] ? atoi(opt->timeout) : 120;
    for (i = 0; i < NUM_AX_CTRL; ++i)
        if (opt->timeout_mask[i])
            tmask |= ax_ctrl_bits[i];
    for (i = 0; i < NUM_AX_OPT; ++i)
        if (opt->options_mask[i])
            omask |= ax_opt_bits[i];

    want_t = (timeout != 120);
    want_m = (tmask != 0x1E);
    want_f = (!opt->feedback);
    want_o = (omask != 0xCEF);

    sent_o = want_o;
    sent_f = want_f || want_o;
    sent_m = want_m || want_f || want_o;
    sent_t = want_t || want_m || want_f || want_o;
    if (!opt->enabled)
        sent_t = sent_m = sent_f = sent_o = 0;

    checkbox_option(ctx, "Enable AccessX key sequences", &opt->enabled,
        "Enable (+) or disable (-) AccessX key sequences (sticky keys, slow\nkeys, and related features).");

    if (!sent_t) dim_push(ctx);
    text_option(ctx, "Timeout (seconds)", opt->timeout, (int)sizeof(opt->timeout),
        "Seconds before AccessX controls turn off automatically (0 = never).");
    if (!sent_t) dim_pop(ctx);

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Timeout mask", NK_TEXT_LEFT);
    if (!sent_m) dim_push(ctx);
    for (i = 0; i < NUM_AX_CTRL; ++i)
        checkbox_option(ctx, ax_ctrl_names[i], &opt->timeout_mask[i],
            ax_ctrl_tips[i]);
    if (!sent_m) dim_pop(ctx);

    nk_label(ctx, "Feedback", NK_TEXT_LEFT);
    if (!sent_f) dim_push(ctx);
    checkbox_option(ctx, "Enable feedback", &opt->feedback,
        "Give audible or visual feedback for AccessX actions.");
    if (!sent_f) dim_pop(ctx);

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Options", NK_TEXT_LEFT);
    if (!sent_o) dim_push(ctx);
    for (i = 0; i < NUM_AX_OPT; ++i)
        checkbox_option(ctx, ax_opt_names[i], &opt->options_mask[i],
            ax_opt_tips[i]);
    if (!sent_o) dim_pop(ctx);
}

struct options_desktop_integration {
    int clipboard_enabled;
    int primary_enabled;
    int codepage_enabled;
    int hostintitle_enabled;
    int trayicon_enabled;
    char icon_spec[256];
    int compositewm_enabled;
    int compositealpha_enabled;
    char clipupdates[16];
};

static void
tab_desktop_integration(struct nk_context *ctx,
    struct options_desktop_integration *opt)
{
    heading(ctx, "Windows desktop integration");

    checkbox_option(ctx, "Clipboard integration (-clipboard)", &opt->clipboard_enabled, "Enable or disable clipboard integration between the X server and\nWindows. Default is enabled.");

    checkbox_option(ctx, "Map PRIMARY selection to clipboard (-primary)", &opt->primary_enabled, "Map the X11 PRIMARY selection to the Windows clipboard when clipboard\nintegration is enabled. The CLIPBOARD selection is always mapped.\nDefault is enabled.");

    checkbox_option(ctx, "Convert ANSI code page (-codepage)", &opt->codepage_enabled, "Convert text between the Windows ANSI code page and ISO-8859-1 for\nlegacy 8-bit X11 clients.");

    checkbox_option(ctx, "Add host names to window titles (-hostintitle)", &opt->hostintitle_enabled, "In -multiwindow mode, add the remote host name to each X11\nwindow's title.");

    checkbox_option(ctx, "Notification-area icon (-trayicon)", &opt->trayicon_enabled, "Do not create a notification area icon. Default is to create one icon\nper screen. Disable all with -notrayicon, then enable specific screens\nwith -trayicon.");

    text_option(ctx, "Window icon (-icon)", opt->icon_spec, (int)sizeof(opt->icon_spec), "Set the screen window icon in windowed mode from the given icon file.");

    checkbox_option(ctx, "Composite extension (-compositewm)", &opt->compositewm_enabled, "Enable or disable the Composite extension (default: enabled). In\n-multiwindow mode this maintains a bitmap of each top-level window, so\noccluded contents show correctly in Taskbar and Task Switcher previews.");

    checkbox_option(ctx, "Composite per-pixel alpha (-compositealpha)", &opt->compositealpha_enabled, "Composite X windows that have per-pixel alpha into the Windows desktop.");

    text_option(ctx, "Clip update boxes (-clipupdates)", opt->clipupdates, (int)sizeof(opt->clipupdates), "Use a clipping region to constrain shadow update blits when there are\nthis many boxes or more in the updated region. Little effect on current\nWindows versions, which already batch GDI operations.");
}

struct options_glx {
    int wgl_enabled;
    int swrastwgl_enabled;
    int iglx_enabled;
};

static void
tab_glx(struct nk_context *ctx, struct options_glx *opt)
{
    heading(ctx, "OpenGL / GLX");

    checkbox_option(ctx, "Native WGL for GLX (-wgl)", &opt->wgl_enabled, "Enable the GLX extension to use the native Windows WGL interface for\nhardware-accelerated OpenGL.");

    checkbox_option(ctx, "WGL swrast for GLX (-swrastwgl)", &opt->swrastwgl_enabled, "Enable the GLX extension to use the native Windows WGL interface with\nthe swrast software renderer.");

    checkbox_option(ctx, "Allow indirect GLX contexts (+iglx)", &opt->iglx_enabled, "+iglx: allow creating indirect GLX contexts (default). -iglx: prohibit\ncreating indirect GLX contexts.");
}

struct options_fonts_rendering {
    char font_path[1024];
    int render_sel;
    int deferglyphs_sel;
    char fakescreenfps[16];
    int backingstore_sel;
    char root_background[16];
    int retro_enabled;
    char color_visual_class[16];
};

static void
tab_fonts_rendering(struct nk_context *ctx, struct options_fonts_rendering *opt)
{
    heading(ctx, "Fonts, rendering & appearance");

    text_option(ctx, "Font path (-fp)", opt->font_path, (int)sizeof(opt->font_path), "Set the search path for fonts. It should be a comma-separated list of\ndirectories and/or font server addresses.");

    combobox_option(ctx, "Render policy (-render)", render_items, RENDER_COUNT, &opt->render_sel, "Set the color allocation policy used by the Render extension. Valid\nvalues: default, mono, gray, color.");

    combobox_option(ctx, "Defer glyphs (-deferglyphs)", deferglyphs_items, DEFERGLYPHS_COUNT, &opt->deferglyphs_sel, "Defer glyph loading for the given font type: none (never), all (every\nfont), or 16 (16-bit fonts only).");

    text_option(ctx, "Fake screen fps (-fakescreenfps)", opt->fakescreenfps, (int)sizeof(opt->fakescreenfps), "Fake the default screen's frame rate to this many frames per second.\nThe default uses the real refresh rate.");

    combobox_option(ctx, "Backing store (+bs/-bs)", backingstore_items, BACKINGSTORE_COUNT, &opt->backingstore_sel, "+bs: enable backing store support on all screens. -bs: disable backing\nstore support on all screens.");

    if (color_option(ctx, "Root background (-bgcolor)", opt->root_background, (int)sizeof(opt->root_background), "Set the root window background color in hexadecimal RRGGBB format."))
        opt->retro_enabled = 0;

    checkbox_option(ctx, "Start with classic stipple (-retro)", &opt->retro_enabled, "Start with the classic stipple pattern and visible cursor. The default is\na black root window.");

    text_option(ctx, "Color visual class (-cc)", opt->color_visual_class, (int)sizeof(opt->color_visual_class), "Set the visual class for the root window of color screens, using the X\nprotocol class number.");
}

struct options_logging_extensions {
    char logfile[1024];
    int logverbose;
    char audit[16];
    int core_enabled;
    int dumbSched_enabled;
    char schedInterval[16];
    char schedMax[16];
    int tst_enabled;
    int extension_enabled[NUM_EXTENSIONS];
};

static void
tab_logging_extensions(struct nk_context *ctx,
    struct options_logging_extensions *opt)
{
    struct nk_rect b;
    int i;

    heading(ctx, "Logging, scheduling & extensions");

    text_option(ctx, "Log file (-logfile)", opt->logfile, (int)sizeof(opt->logfile), "Write log messages to the given file instead of the default log\nlocation.");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Log verbosity (-logverbose)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Set log verbosity: 0 - only fatal errors; 1 - add configuration info;\n2 - add runtime info (default); 3 - add debug and tracing.");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 0, &opt->logverbose, 3, 1);
    option_tooltip(ctx, b, "Set log verbosity: 0 - only fatal errors; 1 - add configuration info;\n2 - add runtime info (default); 3 - add debug and tracing.");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->logverbose);

    checkbox_option(ctx, "Abort on fatal error (-core)", &opt->core_enabled, "Abort on a fatal error and generate a core dump for debugging.");

    text_option(ctx, "Audit trail level (-audit)", opt->audit, (int)sizeof(opt->audit), "Set the audit trail level (default 1). Higher levels record more\nconnection and security events.");

    checkbox_option(ctx, "Disable smart scheduling (-dumbSched)", &opt->dumbSched_enabled, "Disable smart scheduling and threaded input, restoring the old scheduler\nbehavior.");

    text_option(ctx, "Scheduler interval (-schedInterval)", opt->schedInterval, (int)sizeof(opt->schedInterval), "Set the smart scheduler interval in milliseconds.");

    text_option(ctx, "Scheduler max slice (-schedMax)", opt->schedMax, (int)sizeof(opt->schedMax), "Set the smart scheduler's maximum time slice in milliseconds.");

    checkbox_option(ctx, "Disable testing extensions (-tst)", &opt->tst_enabled, "Disable all testing extensions, such as XTEST.");

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Extensions", NK_TEXT_LEFT);
    for (i = 0; i < NUM_EXTENSIONS; ++i) {
        char tip[128];
        snprintf(tip, sizeof(tip),
            "+extension %s: enable this X extension.\n-extension %s: disable it.",
            extension_names[i], extension_names[i]);
        checkbox_option(ctx, extension_names[i], &opt->extension_enabled[i], tip);
    }
}

static void
enumerate_audio_devices(void)
{
    UINT i, n;

    audio_output_device_count = 0;
    n = waveOutGetNumDevs();
    if (n > MAX_AUDIO_DEVICES)
        n = MAX_AUDIO_DEVICES;
    for (i = 0; i < n; i++) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            strncpy(audio_output_devices[audio_output_device_count], caps.szPname, MAXPNAMELEN - 1);
            audio_output_devices[audio_output_device_count][MAXPNAMELEN - 1] = '\0';
            audio_output_device_count++;
        }
    }

    audio_input_device_count = 0;
    n = waveInGetNumDevs();
    if (n > MAX_AUDIO_DEVICES)
        n = MAX_AUDIO_DEVICES;
    for (i = 0; i < n; i++) {
        WAVEINCAPS caps;
        if (waveInGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            strncpy(audio_input_devices[audio_input_device_count], caps.szPname, MAXPNAMELEN - 1);
            audio_input_devices[audio_input_device_count][MAXPNAMELEN - 1] = '\0';
            audio_input_device_count++;
        }
    }
}

static void
device_option(struct nk_context *ctx, const char *label, char *device,
    char names[][MAXPNAMELEN], int nnames, const char *tooltip)
{
    const char *items[MAX_AUDIO_DEVICES + 1];
    int count = nnames + 1;
    int sel = 0;
    int i;

    items[0] = "Auto (system default)";
    for (i = 0; i < nnames; i++)
        items[i + 1] = names[i];
    for (i = 1; i < count; i++) {
        if (strcmp(device, items[i]) == 0) {
            sel = i;
            break;
        }
    }

    combobox_option(ctx, label, items, count, &sel, tooltip);

    if (sel <= 0)
        device[0] = '\0';
    else
        strncpy(device, items[sel], 127), device[127] = '\0';
}

/* Resolve the host's primary IPv4 address for the client env-var hint shown
   when listening on all interfaces. Cached: the dialog re-renders at 60 Hz. */
static int
local_ipv4(char *out, size_t outsize)
{
    static char cached[64];
    static int resolved;            /* 0 = not tried, 1 = ok, -1 = failed */
    int found = 0;

    if (resolved) {
        if (resolved < 0)
            return -1;
        snprintf(out, outsize, "%s", cached);
        return 0;
    }

    /* Preferred: ask the routing table which interface reaches the default
       gateway (0.0.0.0), then take that adapter's IPv4 unicast address. */
    {
        DWORD ifindex = 0;
        IP_ADAPTER_ADDRESSES *list = NULL, *a;

        if (GetBestInterface(0, &ifindex) == NO_ERROR) {
            ULONG buflen = 0;

            if (GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &buflen) ==
                    ERROR_BUFFER_OVERFLOW &&
                (list = (IP_ADAPTER_ADDRESSES *) malloc(buflen)) != NULL) {
                if (GetAdaptersAddresses(AF_INET, 0, NULL, list, &buflen) ==
                        NO_ERROR) {
                    for (a = list; a; a = a->Next) {
                        IP_ADAPTER_UNICAST_ADDRESS *u;

                        if (a->IfIndex != ifindex)
                            continue;
                        for (u = a->FirstUnicastAddress; u; u = u->Next) {
                            struct sockaddr_in *sa;
                            unsigned long addr;

                            if (!u->Address.lpSockaddr ||
                                u->Address.lpSockaddr->sa_family != AF_INET)
                                continue;
                            sa = (struct sockaddr_in *) u->Address.lpSockaddr;
                            addr = ntohl(sa->sin_addr.s_addr);
                            if ((addr >> 24) == 127)          /* loopback */
                                continue;
                            if ((addr & 0xffff0000UL) == 0xa9fe0000UL) /* 169.254 */
                                continue;
                            if (inet_ntop(AF_INET, &sa->sin_addr,
                                          cached, sizeof(cached))) {
                                found = 1;
                                break;
                            }
                        }
                        if (found)
                            break;
                    }
                }
                free(list);
            }
        }
    }

    /* Fallback: resolve the hostname and take the first usable IPv4 address. */
    if (!found) {
        WSADATA wsa;
        char host[256];
        struct addrinfo hints, *res = NULL, *p;

        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            if (gethostname(host, sizeof(host)) == 0) {
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(host, NULL, &hints, &res) == 0) {
                    for (p = res; p != NULL; p = p->ai_next) {
                        struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
                        unsigned long a = ntohl(sa->sin_addr.s_addr);
                        if ((a >> 24) == 127)                   /* loopback */
                            continue;
                        if ((a & 0xffff0000UL) == 0xa9fe0000UL) /* link-local 169.254 */
                            continue;
                        if (inet_ntop(AF_INET, &sa->sin_addr,
                                      cached, sizeof(cached))) {
                            found = 1;
                            break;
                        }
                    }
                    freeaddrinfo(res);
                }
            }
        }
    }

    if (found) {
        resolved = 1;
        snprintf(out, outsize, "%s", cached);
        return 0;
    }
    resolved = -1;
    return -1;
}

static void
tab_audio(struct nk_context *ctx, struct options_audio *opt)
{
    heading(ctx, "Audio");

    {
        char env[128];
        char listen[64] = "127.0.0.1";
        if (opt->listen_sel >= 1)
            local_ipv4(listen, sizeof(listen));
        snprintf(env, sizeof(env), "export PULSE_SERVER=tcp:%s:%s",
            listen, opt->pulseport[0] ? opt->pulseport : "4713");
        readonly_option(ctx, "Client environment variable", env,
            "On Linux, run this command to set the Ming64x as your audio device.");
    }

    {
        char dir[512], path[512] = "";
        if (!config_dir(dir, sizeof dir))
            snprintf(path, sizeof path, "%s\\pulse-cookie", dir);
        readonly_option(ctx, "Data for PULSE_COOKIE file", path,
            "On Linux, run:  export PULSE_COOKIE=~/.ming64x-pulse-cookie\n"
            "Then copy the contents of this file to ~/.ming64x-pulse-cookie\n"
            "This sets up authentication, or set authentication to Anonymous");
    }

    checkbox_option(ctx, "Enable PulseAudio server", &opt->audio_enabled,
        "Start the embedded PulseAudio sound server when the X server launches.\nDisable with -noaudio.");

    if (!opt->audio_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "PulseAudio listen port", opt->pulseport, sizeof(opt->pulseport),
        "TCP port the embedded PulseAudio server listens on (default 4713).");
    combobox_option(ctx, "Listen address", audio_listen_items, AUDIO_LISTEN_COUNT,
        &opt->listen_sel,
        "Bind the PulseAudio TCP server to 127.0.0.1 (loopback only, safest),\n0.0.0.0 (all IPv4 interfaces), or both IPv4 and IPv6. Loopback is\nsufficient when audio is forwarded over an SSH tunnel; choose an\nAll-interfaces option only for LAN clients.");
    combobox_option(ctx, "Authentication", audio_auth_items, AUDIO_AUTH_COUNT,
        &opt->auth_sel,
        "How remote clients authenticate. Cookie requires the shared\npulse-cookie (default); Anonymous accepts any client; IP allow-list\naccepts only the addresses listed below. Note that some client\napplications like FireFox do not support cookies.");
    if (opt->auth_sel == 2) {
        text_option(ctx, "Allowed clients", opt->auth_acl, sizeof(opt->auth_acl),
            "Semicolon-separated IP addresses or CIDR blocks allowed to connect when\nauthentication is set to IP allow-list (e.g. 192.168.1.0/24;10.0.0.5).");
    }
    checkbox_option(ctx, "Microphone", &opt->mic_enabled,
        "Expose the Windows recording device as the PulseAudio source (wavein).");
    checkbox_option(ctx, "Speaker", &opt->speaker_enabled,
        "Expose the Windows playback device as the PulseAudio sink (waveout).");
    device_option(ctx, "Output device", opt->output_device,
        audio_output_devices, audio_output_device_count,
        "Windows playback (waveOut) device to use. Auto selects the system default;\notherwise pick a specific device by name.");
    device_option(ctx, "Input device", opt->input_device,
        audio_input_devices, audio_input_device_count,
        "Windows recording (waveIn) device to use. Auto selects the system default;\notherwise pick a specific device by name.");
    combobox_option(ctx, "Sample rate", audio_rate_items, AUDIO_RATE_COUNT,
        &opt->rate_sel,
        "Sample rate for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (44100 Hz).");
    combobox_option(ctx, "Sample format", audio_format_items, AUDIO_FORMAT_COUNT,
        &opt->format_sel,
        "Bit depth for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (16-bit signed little-endian).");
    combobox_option(ctx, "Channels", audio_channels_items, AUDIO_CHANNELS_COUNT,
        &opt->channels_sel,
        "Channel count for the waveOut/waveIn stream. Auto keeps PulseAudio's\ndefault (stereo).");
    combobox_option(ctx, "Latency", audio_latency_items, AUDIO_LATENCY_COUNT,
        &opt->latency_sel,
        "Audio buffering. Lower latency reduces delay but risks underruns and\nglitches; higher latency is more robust. Maps to the waveOut fragment\ncount and size.");
    checkbox_option(ctx, "Null output", &opt->null_sink_enabled,
        "Load a null (silent) sink so PulseAudio always has a playback device,\neven when no real audio hardware is present.");
    checkbox_option(ctx, "Loopback (mic to speaker)", &opt->loopback_enabled,
        "Route the microphone straight to the speaker (module-loopback) for local\nmonitoring of the recording device.");
    if (!opt->audio_enabled)
        nk_widget_disable_end(ctx);
}

static void
reset_all_options(struct options_ssh_login *ssh_opt,
    struct options_network_and_access_control *net_opt,
    struct options_xdmcp *xdmcp_opt,
    struct options_screen_windowing *screen_opt,
    struct options_pointer_keyboard *pointer_opt,
    struct options_xkb *xkb_opt,
    struct options_accessx *accessx_opt,
    struct options_desktop_integration *desktop_opt,
    struct options_glx *glx_opt,
    struct options_fonts_rendering *fonts_opt,
    struct options_logging_extensions *logging_opt,
    struct options_audio *audio_opt)
{
    *ssh_opt = (struct options_ssh_login) {
        .port = "22",
        .x11_forwarding = 1,
        .save_password = 0,
    };

    *net_opt = (struct options_network_and_access_control) {
        .ac_enabled = 0,
        .allow_string = {0},
        .auth_file = {0},
        .byteswap_enabled = 0,
        .maxclients_sel = 4,      /* 1024 (LIMITCLIENTS) */
        .maxbigreqsize = 4,       /* 4 MB (MAX_BIG_REQUEST_SIZE) */
        .listeningport_sel = 0,   /* :0 (default) */
        .vmid = {0},
        .vsockport = "106000",
        .hyperv_registry_enabled = 0,
    };
    *xdmcp_opt = (struct options_xdmcp) {
        .xdmcp_enabled = 0,
        .query_host = {0},
        .broadcast_enabled = 1,
        .indirect_host = {0},
        .multicast_enabled = 0,
        .port_string = "177",        /* XDM_UDP_PORT */
        .from_address = {0},
        .once_enabled = 0,           /* OneSession = FALSE */
        .display_class = "MIT-unspecified",  /* defaultDisplayClass */
        .cookie = {0},               /* xdmAuthCookie = NULL */
        .display_id = {0},
    };
    *screen_opt = (struct options_screen_windowing) {
        .screens = {0},
        .fullscreen_enabled = 0,
        .rootless_enabled = 0,
        .multiwindow_enabled = 0,
        .nodecoration_enabled = 0,
        .multimonitors_enabled = 0,
        .resize_sel = 2,       /* randr */
        .depth_sel = 0,        /* Auto */
        .refresh = {0},
        .engine_sel = 0,       /* Auto */
        .dpi = {0},
        .xinerama_sel = 0,     /* Disabled */
        .disablexinerama_enabled = 0,
        .lesspointer_enabled = 0,
        .swcursor_enabled = 0,
        .nocursor_enabled = 0,
    };
    *pointer_opt = (struct options_pointer_keyboard) {
        .emulate3buttons_enabled = 0,
        .emulate3buttons_timeout = "50",   /* WIN_DEFAULT_E3B_TIME */
        .winkill_enabled = 1,              /* WIN_DEFAULT_WIN_KILL */
        .unixkill_enabled = 0,
        .keyhook_enabled = 0,
        .ignoreinput_enabled = 0,
        .autorepeat_enabled = 1,           /* DEFAULT_AUTOREPEAT */
        .pointer_acceleration = "2",       /* DEFAULT_PTR_NUMERATOR */
        .pointer_threshold = "4",          /* DEFAULT_PTR_THRESHOLD */
        .bell = 50,                        /* DEFAULT_BELL */
        .autorepeat_delay = "660",         /* XkbDfltRepeatDelay */
        .autorepeat_interval = "40",       /* XkbDfltRepeatInterval */
    };
    *xkb_opt = (struct options_xkb) {
        .xkblayout = {0},
        .xkbmodel = "pc105",
        .xkbvariant = {0},
        .xkboptions = {0},
        .xkbrules = "xorg",
        .xkbdir = {0},
    };
    *accessx_opt = (struct options_accessx) {
        .enabled = 0,
        .timeout = "120",          /* XkbDfltAccessXTimeout */
        .timeout_mask = {
            0,  /* RepeatKeys */
            1,  /* SlowKeys */
            1,  /* BounceKeys */
            1,  /* StickyKeys */
            1,  /* MouseKeys */
            0,  /* MouseKeysAccel */
            0,  /* AccessXKeys */
            0,  /* AccessXTimeout */
            0,  /* AccessXFeedback */
        },
        .feedback = 1,             /* XkbDfltAccessXFeedback */
        .options_mask = {
            1,  /* SKPressFB */
            1,  /* SKAcceptFB */
            1,  /* FeatureFB */
            1,  /* SlowWarnFB */
            0,  /* IndicatorFB */
            1,  /* StickyKeysFB */
            1,  /* TwoKeys */
            1,  /* LatchToLock */
            0,  /* SKReleaseFB */
            0,  /* SKRejectFB */
            1,  /* BKRejectFB */
            1,  /* DumbBellFB */
        },
    };
    *desktop_opt = (struct options_desktop_integration) {
        .clipboard_enabled = 1,      /* g_fClipboard */
        .primary_enabled = 1,        /* fPrimarySelection */
        .codepage_enabled = 0,
        .hostintitle_enabled = 1,    /* g_fHostInTitle */
        .trayicon_enabled = 1,       /* !fNoTrayIcon */
        .icon_spec = {0},
        .compositewm_enabled = 1,    /* fCompositeWM */
        .compositealpha_enabled = 1, /* g_fCompositeAlpha */
        .clipupdates = "0",          /* WIN_DEFAULT_CLIP_UPDATES_NBOXES */
    };
    *glx_opt = (struct options_glx) {
        .wgl_enabled = 1,       /* g_fNativeGl */
        .swrastwgl_enabled = 0, /* g_fswrastwgl */
        .iglx_enabled = 1,      /* enableIndirectGLX */
    };
    *fonts_opt = (struct options_fonts_rendering) {
        .font_path = {0},
        .render_sel = 0,          /* default */
        .deferglyphs_sel = 2,     /* "16" (CACHE_16_BIT_GLYPHS) */
        .fakescreenfps = {0},
        .backingstore_sel = 0,    /* Default */
        .root_background = "0F0F1E",  /* server default root background */
        .retro_enabled = 0,
        .color_visual_class = {0},
    };
    *logging_opt = (struct options_logging_extensions) {
        .logfile = {0},
        .logverbose = 2,           /* g_iLogVerbose */
        .audit = "1",              /* auditTrailLevel */
        .core_enabled = 0,
        .dumbSched_enabled = 0,
        .schedInterval = "2",      /* SMART_SCHEDULE_DEFAULT_INTERVAL */
        .schedMax = "10",          /* SMART_SCHEDULE_MAX_SLICE */
        .tst_enabled = 0,
        .extension_enabled = {
            1, 1, 1, 0,  /* SHAPE, XTEST, SECURITY, XINERAMA(off by default) */
            1, 1, 1, 1,  /* XFIXES, XFree86-Bigfont, RENDER, RANDR */
            1, 1, 1, 1,  /* COMPOSITE, DAMAGE, MIT-SCREEN-SAVER, DOUBLE-BUFFER */
            1, 1, 1, 1,  /* RECORD, DPMS, X-Resource, GLX */
        },
    };
    *audio_opt = (struct options_audio) {
        .audio_enabled = 1,
        .pulseport = "4713",
        .mic_enabled = 1,
        .speaker_enabled = 1,
        .listen_sel = 1,
        .auth_sel = 0,
        .auth_acl = {0},
        .output_device = {0},
        .input_device = {0},
        .rate_sel = 0,
        .format_sel = 0,
        .channels_sel = 0,
        .latency_sel = 0,
        .null_sink_enabled = 0,
        .loopback_enabled = 0,
    };
}

/* ------------------------------------------------------------------ */
/* Configuration persistence: %APPDATA%\Ming64X\launchx.cnf           */
/*                                                                     */
/* One setting per line: "section.element = value\r\n".  The section   */
/* is the lowercased tab name with non-alphanumerics removed, the      */
/* element the lowercased element label with parenthesized text and    */
/* non-alphanumerics removed.  The text after " = " (0x20 0x3D 0x20)   */
/* up to the carriage return is taken literally.                       */
/* ------------------------------------------------------------------ */

struct cfentry {
    const char *section;
    const char *element;
    int is_str;          /* 0 = integer, 1 = string */
    void *value;
    int size;            /* string: buffer size */
    int lo, hi;          /* integer: clamp range */
};

#define CF_STR(sec, el, f)      (struct cfentry){ (sec), (el), 1, (void *)(f), (int)sizeof(f), 0, 0 }
#define CF_BOOL(sec, el, f)     (struct cfentry){ (sec), (el), 0, (void *)&(f), 0, 0, 1 }
#define CF_INT(sec, el, f, lo, hi) (struct cfentry){ (sec), (el), 0, (void *)&(f), 0, (lo), (hi) }

static const char *extension_keys[NUM_EXTENSIONS] = {
    "shape", "xtest", "security", "xinerama", "xfixes",
    "xfree86bigfont", "render", "randr", "composite", "damage",
    "mitscreensaver", "doublebuffer", "record", "dpms",
    "xresource", "glx"
};

static int
cf_build(struct cfentry *e,
    struct options_ssh_login *ssh,
    struct options_network_and_access_control *net,
    struct options_xdmcp *xdmcp,
    struct options_screen_windowing *screen,
    struct options_pointer_keyboard *pointer,
    struct options_xkb *xkb,
    struct options_accessx *accessx,
    struct options_desktop_integration *desktop,
    struct options_glx *glx,
    struct options_fonts_rendering *fonts,
    struct options_logging_extensions *logging,
    struct options_audio *audio)
{
    int n = 0, i;

    e[n++] = CF_STR("sshlogin", "sshconnectip", ssh->host);
    e[n++] = CF_STR("sshlogin", "sshconnectport", ssh->port);
    e[n++] = CF_STR("sshlogin", "loginusername", ssh->username);
    e[n++] = CF_BOOL("sshlogin", "x11forwarding", ssh->x11_forwarding);
    e[n++] = CF_BOOL("sshlogin", "savepassword", ssh->save_password);

    e[n++] = CF_BOOL("networkingaccesscontrol", "disableaccesscontrol", net->ac_enabled);
    e[n++] = CF_STR("networkingaccesscontrol", "allowedipaddresses", net->allow_string);
    e[n++] = CF_STR("networkingaccesscontrol", "authorizationfile", net->auth_file);
    e[n++] = CF_BOOL("networkingaccesscontrol", "allowdifferentendiannessclients", net->byteswap_enabled);
    e[n++] = CF_INT("networkingaccesscontrol", "maxclients", net->maxclients_sel, 0, (int)MAXCLIENTS_COUNT - 1);
    e[n++] = CF_INT("networkingaccesscontrol", "maxbigreqsize", net->maxbigreqsize, 1, 127);
    e[n++] = CF_INT("networkingaccesscontrol", "listeningport", net->listeningport_sel, 0, (int)LISTENINGPORT_COUNT - 1);
    e[n++] = CF_STR("networkingaccesscontrol", "hypervvmguid", net->vmid);
    e[n++] = CF_STR("networkingaccesscontrol", "vsocklistenport", net->vsockport);
    e[n++] = CF_BOOL("networkingaccesscontrol", "updateregistrywithhypervguid", net->hyperv_registry_enabled);

    e[n++] = CF_BOOL("xdmcp", "enablexdmcp", xdmcp->xdmcp_enabled);
    e[n++] = CF_STR("xdmcp", "queryhost", xdmcp->query_host);
    e[n++] = CF_BOOL("xdmcp", "broadcastforxdmcp", xdmcp->broadcast_enabled);
    e[n++] = CF_STR("xdmcp", "indirecthost", xdmcp->indirect_host);
    e[n++] = CF_BOOL("xdmcp", "ipv6multicast", xdmcp->multicast_enabled);
    e[n++] = CF_STR("xdmcp", "udpport", xdmcp->port_string);
    e[n++] = CF_STR("xdmcp", "localaddress", xdmcp->from_address);
    e[n++] = CF_BOOL("xdmcp", "terminateafteronesession", xdmcp->once_enabled);
    e[n++] = CF_STR("xdmcp", "displayclass", xdmcp->display_class);
    e[n++] = CF_STR("xdmcp", "magiccookie", xdmcp->cookie);
    e[n++] = CF_STR("xdmcp", "displayid", xdmcp->display_id);

    e[n++] = CF_STR("screenwindowingmodes", "screen1width", screen->screens[0].width);
    e[n++] = CF_STR("screenwindowingmodes", "screen1height", screen->screens[0].height);
    e[n++] = CF_STR("screenwindowingmodes", "screen1xoffset", screen->screens[0].x);
    e[n++] = CF_STR("screenwindowingmodes", "screen1yoffset", screen->screens[0].y);
    e[n++] = CF_STR("screenwindowingmodes", "screen1monitor", screen->screens[0].monitor);
    e[n++] = CF_STR("screenwindowingmodes", "screen2width", screen->screens[1].width);
    e[n++] = CF_STR("screenwindowingmodes", "screen2height", screen->screens[1].height);
    e[n++] = CF_STR("screenwindowingmodes", "screen2xoffset", screen->screens[1].x);
    e[n++] = CF_STR("screenwindowingmodes", "screen2yoffset", screen->screens[1].y);
    e[n++] = CF_STR("screenwindowingmodes", "screen2monitor", screen->screens[1].monitor);
    e[n++] = CF_STR("screenwindowingmodes", "screen3width", screen->screens[2].width);
    e[n++] = CF_STR("screenwindowingmodes", "screen3height", screen->screens[2].height);
    e[n++] = CF_STR("screenwindowingmodes", "screen3xoffset", screen->screens[2].x);
    e[n++] = CF_STR("screenwindowingmodes", "screen3yoffset", screen->screens[2].y);
    e[n++] = CF_STR("screenwindowingmodes", "screen3monitor", screen->screens[2].monitor);
    e[n++] = CF_BOOL("screenwindowingmodes", "runinfullscreenmode", screen->fullscreen_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "transparentrootwindow", screen->rootless_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "runinmultiwindowmode", screen->multiwindow_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "nowindowbordertitlebar", screen->nodecoration_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "useentirevirtualscreen", screen->multimonitors_enabled);
    e[n++] = CF_INT("screenwindowingmodes", "resizemode", screen->resize_sel, 0, (int)RESIZE_COUNT - 1);
    e[n++] = CF_INT("screenwindowingmodes", "bitdepth", screen->depth_sel, 0, (int)DEPTH_COUNT - 1);
    e[n++] = CF_STR("screenwindowingmodes", "refreshrate", screen->refresh);
    e[n++] = CF_INT("screenwindowingmodes", "engine", screen->engine_sel, 0, (int)ENGINE_COUNT - 1);
    e[n++] = CF_STR("screenwindowingmodes", "screenresolutiondpi", screen->dpi);
    e[n++] = CF_INT("screenwindowingmodes", "xinerama", screen->xinerama_sel, 0, (int)XINERAMA_COUNT - 1);
    e[n++] = CF_BOOL("screenwindowingmodes", "disablexineramaextension", screen->disablexinerama_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "hidewindowspointer", screen->lesspointer_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "x11softwarecursor", screen->swcursor_enabled);
    e[n++] = CF_BOOL("screenwindowingmodes", "disablecursor", screen->nocursor_enabled);

    e[n++] = CF_BOOL("pointerkeyboardinput", "emulate3buttonmouse", pointer->emulate3buttons_enabled);
    e[n++] = CF_STR("pointerkeyboardinput", "emulatetimeout", pointer->emulate3buttons_timeout);
    e[n++] = CF_BOOL("pointerkeyboardinput", "altf4exitsserver", pointer->winkill_enabled);
    e[n++] = CF_BOOL("pointerkeyboardinput", "ctrlaltbackspaceexitsserver", pointer->unixkill_enabled);
    e[n++] = CF_BOOL("pointerkeyboardinput", "grabspecialwindowskeys", pointer->keyhook_enabled);
    e[n++] = CF_BOOL("pointerkeyboardinput", "ignorekeyboardandmouseinput", pointer->ignoreinput_enabled);
    e[n++] = CF_BOOL("pointerkeyboardinput", "enableautorepeat", pointer->autorepeat_enabled);
    e[n++] = CF_STR("pointerkeyboardinput", "pointeracceleration", pointer->pointer_acceleration);
    e[n++] = CF_STR("pointerkeyboardinput", "pointerthreshold", pointer->pointer_threshold);
    e[n++] = CF_INT("pointerkeyboardinput", "bellbase", pointer->bell, 0, 100);
    e[n++] = CF_STR("pointerkeyboardinput", "autorepeatdelay", pointer->autorepeat_delay);
    e[n++] = CF_STR("pointerkeyboardinput", "autorepeatinterval", pointer->autorepeat_interval);

    e[n++] = CF_STR("xkbkeyboardlayout", "layout", xkb->xkblayout);
    e[n++] = CF_STR("xkbkeyboardlayout", "model", xkb->xkbmodel);
    e[n++] = CF_STR("xkbkeyboardlayout", "variant", xkb->xkbvariant);
    e[n++] = CF_STR("xkbkeyboardlayout", "options", xkb->xkboptions);
    e[n++] = CF_STR("xkbkeyboardlayout", "rules", xkb->xkbrules);
    e[n++] = CF_STR("xkbkeyboardlayout", "basedirectory", xkb->xkbdir);

    e[n++] = CF_BOOL("accessxkeysequence", "accessx.enabled", accessx->enabled);
    e[n++] = CF_STR("accessxkeysequence", "accessx.timeout", accessx->timeout);
    for (i = 0; i < NUM_AX_CTRL; ++i)
        e[n++] = CF_BOOL("accessxkeysequence", ax_ctrl_keys[i], accessx->timeout_mask[i]);
    e[n++] = CF_BOOL("accessxkeysequence", "accessx.feedback", accessx->feedback);
    for (i = 0; i < NUM_AX_OPT; ++i)
        e[n++] = CF_BOOL("accessxkeysequence", ax_opt_keys[i], accessx->options_mask[i]);

    e[n++] = CF_BOOL("windowsdesktopintegration", "clipboardintegration", desktop->clipboard_enabled);
    e[n++] = CF_BOOL("windowsdesktopintegration", "mapprimaryselectiontoclipboard", desktop->primary_enabled);
    e[n++] = CF_BOOL("windowsdesktopintegration", "convertansicodepage", desktop->codepage_enabled);
    e[n++] = CF_BOOL("windowsdesktopintegration", "addhostnamestowindowtitles", desktop->hostintitle_enabled);
    e[n++] = CF_BOOL("windowsdesktopintegration", "notificationareaicon", desktop->trayicon_enabled);
    e[n++] = CF_STR("windowsdesktopintegration", "windowicon", desktop->icon_spec);
    e[n++] = CF_BOOL("windowsdesktopintegration", "compositeextension", desktop->compositewm_enabled);
    e[n++] = CF_BOOL("windowsdesktopintegration", "compositeperpixelalpha", desktop->compositealpha_enabled);
    e[n++] = CF_STR("windowsdesktopintegration", "clipupdateboxes", desktop->clipupdates);

    e[n++] = CF_BOOL("openglglx", "nativewglforglx", glx->wgl_enabled);
    e[n++] = CF_BOOL("openglglx", "wglswrastforglx", glx->swrastwgl_enabled);
    e[n++] = CF_BOOL("openglglx", "allowindirectglxcontexts", glx->iglx_enabled);

    e[n++] = CF_STR("fontsrenderingappearance", "fontpath", fonts->font_path);
    e[n++] = CF_INT("fontsrenderingappearance", "renderpolicy", fonts->render_sel, 0, (int)RENDER_COUNT - 1);
    e[n++] = CF_INT("fontsrenderingappearance", "deferglyphs", fonts->deferglyphs_sel, 0, (int)DEFERGLYPHS_COUNT - 1);
    e[n++] = CF_STR("fontsrenderingappearance", "fakescreenfps", fonts->fakescreenfps);
    e[n++] = CF_INT("fontsrenderingappearance", "backingstore", fonts->backingstore_sel, 0, (int)BACKINGSTORE_COUNT - 1);
    e[n++] = CF_STR("fontsrenderingappearance", "rootbackground", fonts->root_background);
    e[n++] = CF_BOOL("fontsrenderingappearance", "startwithclassicstipple", fonts->retro_enabled);
    e[n++] = CF_STR("fontsrenderingappearance", "colorvisualclass", fonts->color_visual_class);

    e[n++] = CF_STR("loggingschedulingextensions", "logfile", logging->logfile);
    e[n++] = CF_INT("loggingschedulingextensions", "logverbosity", logging->logverbose, 0, 3);
    e[n++] = CF_BOOL("loggingschedulingextensions", "abortonfatalerror", logging->core_enabled);
    e[n++] = CF_STR("loggingschedulingextensions", "audittraillevel", logging->audit);
    e[n++] = CF_BOOL("loggingschedulingextensions", "disablesmartscheduling", logging->dumbSched_enabled);
    e[n++] = CF_STR("loggingschedulingextensions", "schedulerinterval", logging->schedInterval);
    e[n++] = CF_STR("loggingschedulingextensions", "schedulermaxslice", logging->schedMax);
    e[n++] = CF_BOOL("loggingschedulingextensions", "disabletestingextensions", logging->tst_enabled);
    for (i = 0; i < NUM_EXTENSIONS; ++i)
        e[n++] = CF_BOOL("loggingschedulingextensions", extension_keys[i], logging->extension_enabled[i]);

    e[n++] = CF_BOOL("audio", "enablepulseaudioserver", audio->audio_enabled);
    e[n++] = CF_STR("audio", "pulseaudiolistenport", audio->pulseport);
    e[n++] = CF_INT("audio", "listenaddress", audio->listen_sel, 0, AUDIO_LISTEN_COUNT - 1);
    e[n++] = CF_INT("audio", "authentication", audio->auth_sel, 0, AUDIO_AUTH_COUNT - 1);
    e[n++] = CF_STR("audio", "allowedclients", audio->auth_acl);
    e[n++] = CF_BOOL("audio", "microphone", audio->mic_enabled);
    e[n++] = CF_BOOL("audio", "speaker", audio->speaker_enabled);
    e[n++] = CF_STR("audio", "outputdevice", audio->output_device);
    e[n++] = CF_STR("audio", "inputdevice", audio->input_device);
    e[n++] = CF_INT("audio", "samplerate", audio->rate_sel, 0, AUDIO_RATE_COUNT - 1);
    e[n++] = CF_INT("audio", "sampleformat", audio->format_sel, 0, AUDIO_FORMAT_COUNT - 1);
    e[n++] = CF_INT("audio", "channels", audio->channels_sel, 0, AUDIO_CHANNELS_COUNT - 1);
    e[n++] = CF_INT("audio", "latency", audio->latency_sel, 0, AUDIO_LATENCY_COUNT - 1);
    e[n++] = CF_BOOL("audio", "nulloutput", audio->null_sink_enabled);
    e[n++] = CF_BOOL("audio", "loopback", audio->loopback_enabled);

    return n;
}

static int
config_dir(char *out, const size_t outsz)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !appdata[0])
        return -1;
    snprintf(out, outsz, "%s\\Ming64X", appdata);
    return 0;
}

static void
password_crypt_setup(struct blf_ctx *c)
{
    unsigned char key[PASSWORD_KEY_LEN];
    GET_PASSWORD_KEY(key);
    Blowfish_initstate(c);
    Blowfish_expand0state(c, key, PASSWORD_KEY_LEN);
}

static void
password_block_xor(uint32_t *a, const uint32_t *b)
{
    a[0] ^= b[0];
    a[1] ^= b[1];
}

static void
password_block_hex_encode(const uint32_t block[2], char *out)
{
    static const char hexd[] = "0123456789abcdef";
    const unsigned char *b = (const unsigned char *)block;
    int i;
    for (i = 0; i < 8; ++i) {
        out[i * 2]     = hexd[b[i] >> 4];
        out[i * 2 + 1] = hexd[b[i] & 0x0F];
    }
    out[16] = '\0';
}

static int
password_block_hex_decode(const char *in, uint32_t block[2])
{
    unsigned char *b = (unsigned char *)block;
    int i;
    for (i = 0; i < 8; ++i) {
        int hi = in[i * 2], lo = in[i * 2 + 1];
        int vhi, vlo;
        if (hi >= '0' && hi <= '9')      vhi = hi - '0';
        else if (hi >= 'a' && hi <= 'f') vhi = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') vhi = hi - 'A' + 10;
        else return 0;
        if (lo >= '0' && lo <= '9')      vlo = lo - '0';
        else if (lo >= 'a' && lo <= 'f') vlo = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') vlo = lo - 'A' + 10;
        else return 0;
        b[i] = (unsigned char)((vhi << 4) | vlo);
    }
    return 1;
}

static int
password_encrypt_hex(const char *plain, char *hex, size_t hexsz)
{
    struct blf_ctx c;
    uint32_t prev[2], block[2];
    unsigned char iv[IV_BLOCK_LEN];
    unsigned char buf[256];
    size_t plen, padded, off;

    plen = strlen(plain);
    padded = plen + (8 - (plen % 8));
    if (padded > sizeof(buf))
        return -1;
    if (padded / 8 * 16 + 1 > hexsz)
        return -1;

    memset(buf, 0, sizeof(buf));
    memcpy(buf, plain, plen);

    password_crypt_setup(&c);
    GET_IV(iv);
    memcpy(prev, iv, IV_BLOCK_LEN);

    for (off = 0; off < padded; off += 8) {
        memcpy(block, buf + off, 8);
        password_block_xor(block, prev);
        Blowfish_encipher(&c, &block[0], &block[1]);
        password_block_hex_encode(block, hex + (off / 8) * 16);
        memcpy(prev, block, 8);
    }
    hex[padded / 8 * 16] = '\0';
    return 0;
}

static int
password_decrypt_hex(const char *hex, char *plain, size_t plainsz)
{
    struct blf_ctx c;
    uint32_t prev[2], block[2];
    unsigned char iv[IV_BLOCK_LEN];
    unsigned char buf[256];
    size_t hexlen, bytes, off, plen;

    hexlen = strlen(hex);
    if (hexlen == 0 || hexlen % 16 != 0)
        return -1;
    bytes = hexlen / 2;
    if (bytes > sizeof(buf))
        bytes = sizeof(buf);

    password_crypt_setup(&c);
    GET_IV(iv);
    memcpy(prev, iv, IV_BLOCK_LEN);

    for (off = 0; off + 8 <= bytes; off += 8) {
        uint32_t cipher[2];
        if (!password_block_hex_decode(hex + (off / 8) * 16, block))
            return -1;
        memcpy(cipher, block, 8);
        Blowfish_decipher(&c, &block[0], &block[1]);
        password_block_xor(block, prev);
        memcpy(buf + off, block, 8);
        memcpy(prev, cipher, 8);
    }
    buf[off] = '\0';

    plen = strlen((char *)buf);
    if (plen >= plainsz)
        plen = plainsz - 1;
    memcpy(plain, buf, plen);
    plain[plen] = '\0';
    return 0;
}

static void
save_config(struct options_ssh_login *ssh,
    struct options_network_and_access_control *net,
    struct options_xdmcp *xdmcp,
    struct options_screen_windowing *screen,
    struct options_pointer_keyboard *pointer,
    struct options_xkb *xkb,
    struct options_accessx *accessx,
    struct options_desktop_integration *desktop,
    struct options_glx *glx,
    struct options_fonts_rendering *fonts,
    struct options_logging_extensions *logging,
    struct options_audio *audio)
{
    struct cfentry e[160];
    int n = cf_build(e, ssh, net, xdmcp, screen, pointer, xkb, accessx, desktop, glx, fonts, logging, audio);
    char dir[512], path[512], tmp[512];
    FILE *f;
    int i;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\launchx.cnf", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    f = fopen(tmp, "wb");
    if (!f)
        return;
    for (i = 0; i < n; ++i) {
        if (e[i].is_str)
            fprintf(f, "%s.%s = %s\r\n", e[i].section, e[i].element,
                (const char *)e[i].value);
        else
            fprintf(f, "%s.%s = %d\r\n", e[i].section, e[i].element,
                *(int *)e[i].value);
    }
    {
        char hex[512];
        if (ssh->save_password &&
            password_encrypt_hex(ssh->password, hex, sizeof hex) == 0)
            fprintf(f, "sshlogin.loginpassword = %s\r\n", hex);
    }
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static void
load_config(struct options_ssh_login *ssh,
    struct options_network_and_access_control *net,
    struct options_xdmcp *xdmcp,
    struct options_screen_windowing *screen,
    struct options_pointer_keyboard *pointer,
    struct options_xkb *xkb,
    struct options_accessx *accessx,
    struct options_desktop_integration *desktop,
    struct options_glx *glx,
    struct options_fonts_rendering *fonts,
    struct options_logging_extensions *logging,
    struct options_audio *audio)
{
    struct cfentry e[160];
    int n = cf_build(e, ssh, net, xdmcp, screen, pointer, xkb, accessx, desktop, glx, fonts, logging, audio);
    char dir[512], path[512], line[4096];
    FILE *f;
    int i;

    if (config_dir(dir, sizeof dir))
        return;
    snprintf(path, sizeof path, "%s\\launchx.cnf", dir);

    f = fopen(path, "rb");
    if (!f)
        return;

    while (fgets(line, sizeof line, f)) {
        char *eq, *dot, *val;
        size_t len;

        len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        eq = strstr(line, " = ");
        if (!eq)
            continue;
        dot = strchr(line, '.');
        if (!dot || dot > eq)
            continue;
        val = eq + 3;
        *dot = '\0';
        *eq = '\0';

        if (strcmp(line, "sshlogin") == 0 &&
            strcmp(dot + 1, "loginpassword") == 0) {
            password_decrypt_hex(val, ssh->password, sizeof(ssh->password));
            continue;
        }

        for (i = 0; i < n; ++i) {
            if (strcmp(e[i].section, line) == 0 &&
                strcmp(e[i].element, dot + 1) == 0) {
                if (e[i].is_str) {
                    strncpy((char *)e[i].value, val, e[i].size - 1);
                    ((char *)e[i].value)[e[i].size - 1] = '\0';
                } else {
                    int v = atoi(val);
                    if (v < e[i].lo) v = e[i].lo;
                    if (v > e[i].hi) v = e[i].hi;
                    *(int *)e[i].value = v;
                }
                break;
            }
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Sanity-check the loaded configuration.  A hand-edited launchx.cnf    */
/* can set options that are mutually exclusive on the server command   */
/* line (see winValidateArgs).  Coerce such combinations to a valid    */
/* state, keeping the primary screen mode and dropping the modifier.    */
/* Returns non-zero if anything was changed.                            */
/* ------------------------------------------------------------------ */
static int
sanitize_options(struct options_xdmcp *xdmcp,
    struct options_screen_windowing *screen)
{
    int changed = 0;

    /* -screen geometry is mutually exclusive with the screen modes. */
    if (screen_geometry_set(screen)) {
        if (screen->multiwindow_enabled) { screen->multiwindow_enabled = 0; changed = 1; }
        if (screen->rootless_enabled) { screen->rootless_enabled = 0; changed = 1; }
        if (screen->fullscreen_enabled) { screen->fullscreen_enabled = 0; changed = 1; }
        if (screen->multimonitors_enabled) { screen->multimonitors_enabled = 0; changed = 1; }
    }

    /* -multiwindow, -rootless and -fullscreen are mutually exclusive. */
    if (screen->multiwindow_enabled && screen->rootless_enabled) {
        screen->rootless_enabled = 0;
        changed = 1;
    }
    if (screen->multiwindow_enabled && screen->fullscreen_enabled) {
        screen->fullscreen_enabled = 0;
        changed = 1;
    }
    if (screen->rootless_enabled && screen->fullscreen_enabled) {
        screen->fullscreen_enabled = 0;
        changed = 1;
    }

    /* -nodecoration is invalid with any of the exclusive screen modes. */
    if (screen->nodecoration_enabled &&
        (screen->multiwindow_enabled || screen->rootless_enabled ||
         screen->fullscreen_enabled)) {
        screen->nodecoration_enabled = 0;
        changed = 1;
    }

    /* -fullscreen is invalid with -lesspointer and a non-none resize. */
    if (screen->fullscreen_enabled) {
        if (screen->lesspointer_enabled) {
            screen->lesspointer_enabled = 0;
            changed = 1;
        }
        if (screen->resize_sel == 1) {           /* scrollbars */
            screen->resize_sel = 2;              /* randr (server default) */
            changed = 1;
        }
    }

    /* -depth and -refresh are only valid with -fullscreen. */
    if (!screen->fullscreen_enabled) {
        if (screen->depth_sel != 0) {            /* not Auto */
            screen->depth_sel = 0;               /* Auto */
            changed = 1;
        }
        if (screen->refresh[0] != '\0') {
            screen->refresh[0] = '\0';
            changed = 1;
        }
    }

    /* XDMCP is invalid with -multiwindow. */
    if (screen->multiwindow_enabled && xdmcp->xdmcp_enabled) {
        xdmcp->xdmcp_enabled = 0;
        changed = 1;
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/* Command-line construction for ming64x.exe.                          */
/*                                                                     */
/* Builds the server command line from the option structs, emitting a  */
/* flag only when its value differs from the built-in server default   */
/* (the defaults chosen in reset_all_options mirror the server's).      */
/* ------------------------------------------------------------------ */

struct cmdline {
    char buf[16384];
    size_t len;
};

static void
cl_init(struct cmdline *c)
{
    c->len = 0;
    c->buf[0] = '\0';
}

/* Append one argument, quoting it when it contains spaces/tabs/quotes
   (backslash-aware so Windows paths round-trip through the CRT argv
   parser used by the server). */
static void
cl_arg(struct cmdline *c, const char *arg)
{
    const char *p;
    int need_quote = (*arg == '\0');

    if (c->len)
        c->buf[c->len++] = ' ';

    if (!need_quote) {
        for (p = arg; *p; ++p) {
            if (*p == ' ' || *p == '\t' || *p == '"') {
                need_quote = 1;
                break;
            }
        }
    }

    if (!need_quote) {
        size_t n = strlen(arg);
        memcpy(c->buf + c->len, arg, n);
        c->len += n;
        return;
    }

    c->buf[c->len++] = '"';
    p = arg;
    while (*p) {
        size_t nbs = 0;
        while (*p == '\\') {
            ++nbs;
            ++p;
        }
        if (*p == '"') {
            size_t i;
            for (i = 0; i < nbs; ++i)
                c->buf[c->len++] = '\\';
            for (i = 0; i < nbs; ++i)
                c->buf[c->len++] = '\\';
            c->buf[c->len++] = '\\';
            c->buf[c->len++] = '"';
            ++p;
        } else if (*p == '\0') {
            size_t i;
            for (i = 0; i < nbs; ++i)
                c->buf[c->len++] = '\\';
            for (i = 0; i < nbs; ++i)
                c->buf[c->len++] = '\\';
            break;
        } else {
            size_t i;
            for (i = 0; i < nbs; ++i)
                c->buf[c->len++] = '\\';
            c->buf[c->len++] = *p++;
        }
    }
    c->buf[c->len++] = '"';
}

/* Append "flag value" only when value is non-empty and != def. */
static void
cl_opt(struct cmdline *c, const char *flag, const char *val, const char *def)
{
    if (val[0] == '\0')
        return;
    if (def && strcmp(val, def) == 0)
        return;
    cl_arg(c, flag);
    cl_arg(c, val);
}

/* Append a bare flag when cond is set. */
static void
cl_if(struct cmdline *c, const int cond, const char *flag)
{
    if (cond)
        cl_arg(c, flag);
}

static void
build_server_cmdline(struct cmdline *c,
    struct options_ssh_login *ssh_opt,
    ssh_session *ssh,
    struct options_network_and_access_control *net,
    struct options_xdmcp *xdmcp,
    struct options_screen_windowing *screen,
    struct options_pointer_keyboard *pointer,
    struct options_xkb *xkb,
    struct options_accessx *accessx,
    struct options_desktop_integration *desktop,
    struct options_glx *glx,
    struct options_fonts_rendering *fonts,
    struct options_logging_extensions *logging,
    struct options_audio *audio)
{
    char tmp[32];

    cl_init(c);
    cl_arg(c, "ming64x.exe");

    if (net->listeningport_sel != 0) {
        char disp[8];
        snprintf(disp, sizeof disp, ":%d", net->listeningport_sel);
        cl_arg(c, disp);
    }

    /* Networking & access control */
    cl_if(c, net->ac_enabled, "-ac");
    /* With access control enabled and a live SSH session, let the server
       accept the X11-forwarded connections arriving from the SSH host. */
    {
        char allow_buf[sizeof(net->allow_string) + 128 + 2];
        const char *allow = net->allow_string;
        if (!net->ac_enabled && ssh_opt->host[0] &&
            ssh_session_is_active(ssh)) {
            if (allow[0])
                snprintf(allow_buf, sizeof allow_buf, "%s,%s", allow,
                    ssh_opt->host);
            else
                snprintf(allow_buf, sizeof allow_buf, "%s", ssh_opt->host);
            allow = allow_buf;
        }
        cl_opt(c, "-allow", allow, NULL);
    }
    cl_opt(c, "-auth", net->auth_file, NULL);
    cl_if(c, net->byteswap_enabled, "+byteswappedclients");
    if (net->maxclients_sel != 4) {
        cl_arg(c, "-maxclients");
        cl_arg(c, maxclients_items[net->maxclients_sel]);
    }
    if (net->maxbigreqsize != 4) {
        snprintf(tmp, sizeof tmp, "%d", net->maxbigreqsize);
        cl_arg(c, "-maxbigreqsize");
        cl_arg(c, tmp);
    }
    cl_opt(c, "-vmid", net->vmid, NULL);
    cl_opt(c, "-vsockport", net->vsockport, "106000");

    /* XDMCP */
    if (xdmcp->xdmcp_enabled) {
        if (xdmcp->broadcast_enabled)
            cl_arg(c, "-broadcast");
        else
            cl_opt(c, "-query", xdmcp->query_host, NULL);
        cl_opt(c, "-indirect", xdmcp->indirect_host, NULL);
        cl_if(c, xdmcp->multicast_enabled, "-multicast");
        cl_opt(c, "-port", xdmcp->port_string, "177");
        cl_opt(c, "-from", xdmcp->from_address, NULL);
        cl_if(c, xdmcp->once_enabled, "-once");
        cl_opt(c, "-class", xdmcp->display_class, "MIT-unspecified");
        cl_opt(c, "-cookie", xdmcp->cookie, NULL);
        cl_opt(c, "-displayID", xdmcp->display_id, NULL);
    }

    /* Screen & windowing modes */
    {
        int scr = screen_geometry_set(screen);
        int counter = 0;

        if (scr && !screen->fullscreen_enabled &&
            !screen->rootless_enabled && !screen->multiwindow_enabled &&
            !screen->multimonitors_enabled) {
            int si;
            for (si = 0; si < 3; ++si) {
                struct screen_geometry_entry *g = &screen->screens[si];
                int w = g->width[0]   ? atoi(g->width)   : 0;
                int h = g->height[0]  ? atoi(g->height)  : 0;
                int x = g->x[0]       ? atoi(g->x)       : 0;
                int y = g->y[0]       ? atoi(g->y)       : 0;
                int m = g->monitor[0] ? atoi(g->monitor) : 0;
                int has_w = g->width[0]   != '\0';
                int has_h = g->height[0]  != '\0';
                int has_x = g->x[0]       != '\0';
                int has_y = g->y[0]       != '\0';
                int has_m = g->monitor[0] != '\0';
                char geo[64];
                char num[2];
                int off = 0;

                if (!has_w && has_h) { w = h * 3 / 2; has_w = 1; }
                if (!has_h && has_w) { h = w * 2 / 3; has_h = 1; }
                if (!has_x && has_y) { x = 0; has_x = 1; }
                if (!has_y && has_x) { y = 0; has_y = 1; }
                if ((has_x || has_y) && !has_w && !has_h) { has_x = 0; has_y = 0; }

                if (!has_w && !has_h && !has_x && !has_y && !has_m)
                    continue;

                geo[0] = '\0';
                if (has_w && has_h)
                    off += snprintf(geo + off, sizeof(geo) - off, "%dx%d", w, h);
                if (has_x && has_y)
                    off += snprintf(geo + off, sizeof(geo) - off, "+%d+%d", x, y);
                if (has_m)
                    off += snprintf(geo + off, sizeof(geo) - off, "@%d", m);

                num[0] = (char)('0' + counter);
                num[1] = '\0';
                cl_arg(c, "-screen");
                cl_arg(c, num);
                cl_arg(c, geo);
                counter++;
            }
        }

        cl_if(c, screen->fullscreen_enabled && !scr, "-fullscreen");
        cl_if(c, screen->rootless_enabled && !scr, "-rootless");
        cl_if(c, screen->multiwindow_enabled && !scr, "-multiwindow");
        cl_if(c, screen->nodecoration_enabled, "-nodecoration");
        cl_if(c, screen->multimonitors_enabled && !scr, "-multimonitors");
    }
    if (screen->resize_sel == 0)
        cl_arg(c, "-resize=none");
    else if (screen->resize_sel == 1)
        cl_arg(c, "-resize=scrollbars");
    if (screen->depth_sel != 0) {
        cl_arg(c, "-depth");
        cl_arg(c, depth_items[screen->depth_sel]);
    }
    cl_opt(c, "-refresh", screen->refresh, NULL);
    if (screen->engine_sel == 1) {
        cl_arg(c, "-engine");
        cl_arg(c, "1");
    } else if (screen->engine_sel == 2) {
        cl_arg(c, "-engine");
        cl_arg(c, "4");
    }
    cl_opt(c, "-dpi", screen->dpi, NULL);
    if (screen->xinerama_sel == 1)
        cl_arg(c, "+xinerama");
    cl_if(c, screen->disablexinerama_enabled, "-disablexineramaextension");
    cl_if(c, screen->lesspointer_enabled, "-lesspointer");
    cl_if(c, screen->swcursor_enabled, "-swcursor");
    cl_if(c, screen->nocursor_enabled, "-nocursor");

    /* Pointer & keyboard input */
    if (pointer->emulate3buttons_enabled) {
        cl_arg(c, "-emulate3buttons");
        if (pointer->emulate3buttons_timeout[0] &&
            strcmp(pointer->emulate3buttons_timeout, "50") != 0)
            cl_arg(c, pointer->emulate3buttons_timeout);
    }
    if (!pointer->winkill_enabled)
        cl_arg(c, "-nowinkill");
    cl_if(c, pointer->unixkill_enabled, "-unixkill");
    cl_if(c, pointer->keyhook_enabled, "-keyhook");
    cl_if(c, pointer->ignoreinput_enabled, "-ignoreinput");
    if (!pointer->autorepeat_enabled)
        cl_arg(c, "-r");
    cl_opt(c, "-a", pointer->pointer_acceleration, "2");
    cl_opt(c, "-t", pointer->pointer_threshold, "4");
    if (pointer->bell != 50) {
        snprintf(tmp, sizeof tmp, "%d", pointer->bell);
        cl_arg(c, "-f");
        cl_arg(c, tmp);
    }
    cl_opt(c, "-ardelay", pointer->autorepeat_delay, "660");
    cl_opt(c, "-arinterval", pointer->autorepeat_interval, "40");

    /* AccessX key sequences */
    if (accessx->enabled) {
        unsigned int tmask = 0, omask = 0;
        int timeout = accessx->timeout[0] ? atoi(accessx->timeout) : 120;
        int want_t, want_m, want_f, want_o, i;

        for (i = 0; i < NUM_AX_CTRL; ++i)
            if (accessx->timeout_mask[i])
                tmask |= ax_ctrl_bits[i];
        for (i = 0; i < NUM_AX_OPT; ++i)
            if (accessx->options_mask[i])
                omask |= ax_opt_bits[i];

        want_t = (timeout != 120);
        want_m = (tmask != 0x1E);
        want_f = (!accessx->feedback);
        want_o = (omask != 0xCEF);

        cl_arg(c, "+accessx");
        if (want_t || want_m || want_f || want_o) {
            snprintf(tmp, sizeof tmp, "%d", timeout);
            cl_arg(c, tmp);
        }
        if (want_m || want_f || want_o) {
            snprintf(tmp, sizeof tmp, "0x%X", tmask);
            cl_arg(c, tmp);
        }
        if (want_f || want_o)
            cl_arg(c, accessx->feedback ? "1" : "0");
        if (want_o) {
            snprintf(tmp, sizeof tmp, "0x%X", omask);
            cl_arg(c, tmp);
        }
    }

    /* XKB keyboard layout */
    cl_opt(c, "-xkblayout", xkb->xkblayout, NULL);
    cl_opt(c, "-xkbmodel", xkb->xkbmodel, "pc105");
    cl_opt(c, "-xkbvariant", xkb->xkbvariant, NULL);
    cl_opt(c, "-xkboptions", xkb->xkboptions, NULL);
    cl_opt(c, "-xkbrules", xkb->xkbrules, "xorg");
    cl_opt(c, "-xkbdir", xkb->xkbdir, NULL);

    /* Windows desktop integration */
    if (!desktop->clipboard_enabled)
        cl_arg(c, "-noclipboard");
    if (!desktop->primary_enabled)
        cl_arg(c, "-noprimary");
    cl_if(c, desktop->codepage_enabled, "-codepage");
    if (!desktop->hostintitle_enabled)
        cl_arg(c, "-nohostintitle");
    if (!desktop->trayicon_enabled)
        cl_arg(c, "-notrayicon");
    cl_opt(c, "-icon", desktop->icon_spec, NULL);
    if (!desktop->compositewm_enabled)
        cl_arg(c, "-nocompositewm");
    if (!desktop->compositealpha_enabled)
        cl_arg(c, "-nocompositealpha");
    cl_opt(c, "-clipupdates", desktop->clipupdates, "0");

    /* Audio */
    if (!audio->audio_enabled)
        cl_arg(c, "-noaudio");

    /* OpenGL / GLX */
    if (!glx->wgl_enabled)
        cl_arg(c, "-nowgl");
    cl_if(c, glx->swrastwgl_enabled, "-swrastwgl");
    if (!glx->iglx_enabled)
        cl_arg(c, "-iglx");

    /* Fonts, rendering & appearance */
    cl_opt(c, "-fp", fonts->font_path, NULL);
    if (fonts->render_sel != 0) {
        cl_arg(c, "-render");
        cl_arg(c, render_items[fonts->render_sel]);
    }
    if (fonts->deferglyphs_sel != 2) {
        cl_arg(c, "-deferglyphs");
        cl_arg(c, deferglyphs_items[fonts->deferglyphs_sel]);
    }
    cl_opt(c, "-fakescreenfps", fonts->fakescreenfps, NULL);
    if (fonts->backingstore_sel == 1)
        cl_arg(c, "+bs");
    else if (fonts->backingstore_sel == 2)
        cl_arg(c, "-bs");
    if (strcmp(fonts->root_background, "0F0F1E") != 0) {
        cl_arg(c, "-bgcolor");
        cl_arg(c, fonts->root_background);
    }
    cl_if(c, fonts->retro_enabled, "-retro");
    cl_opt(c, "-cc", fonts->color_visual_class, NULL);

    /* Logging, scheduling & extensions */
    cl_opt(c, "-logfile", logging->logfile, NULL);
    if (logging->logverbose != 2) {
        snprintf(tmp, sizeof tmp, "%d", logging->logverbose);
        cl_arg(c, "-logverbose");
        cl_arg(c, tmp);
    }
    cl_opt(c, "-audit", logging->audit, "1");
    cl_if(c, logging->core_enabled, "-core");
    cl_if(c, logging->dumbSched_enabled, "-dumbSched");
    cl_opt(c, "-schedInterval", logging->schedInterval, "2");
    cl_opt(c, "-schedMax", logging->schedMax, "10");
    cl_if(c, logging->tst_enabled, "-tst");
    {
        static const int ext_default[NUM_EXTENSIONS] = {
            1, 1, 1, 0,  /* SHAPE, XTEST, SECURITY, XINERAMA(off by default) */
            1, 1, 1, 1,  /* XFIXES, XFree86-Bigfont, RENDER, RANDR */
            1, 1, 1, 1,  /* COMPOSITE, DAMAGE, MIT-SCREEN-SAVER, DOUBLE-BUFFER */
            1, 1, 1, 1,  /* RECORD, DPMS, X-Resource, GLX */
        };
        int i;
        for (i = 0; i < NUM_EXTENSIONS; ++i) {
            if (logging->extension_enabled[i] != ext_default[i]) {
                cl_arg(c, logging->extension_enabled[i]
                    ? "+extension" : "-extension");
                cl_arg(c, extension_names[i]);
            }
        }
    }

    c->buf[c->len] = '\0';
}

static void
write_commandline_file(const char *cmdline)
{
    char dir[512], path[512];
    FILE *f;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\commandline.txt", dir);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "%s\r\n", cmdline);
    fclose(f);
}

static void
write_default_pa(struct options_audio *a)
{
    char dir[512], path[512], cookie[512], cookie_esc[512];
    FILE *f;
    const char *s;
    char *d;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\default.pa", dir);
    snprintf(cookie, sizeof cookie, "%s\\pulse-cookie", dir);

    /* load-module args are unescaped by pa_unescape, which drops every
       backslash, so a Windows path must be written with each one doubled. */
    d = cookie_esc;
    for (s = cookie; *s && d < cookie_esc + sizeof cookie_esc - 1; s++) {
        if (*s == '\\')
            *d++ = '\\';
        *d++ = *s;
    }
    *d = '\0';

    f = fopen(path, "wb");
    if (!f)
        return;
    if (a->speaker_enabled || a->mic_enabled) {
        fputs("load-module module-waveout", f);
        fputs(a->speaker_enabled ? " playback=1" : " playback=0", f);
        fputs(" sink_name=waveout", f);
        fputs(a->mic_enabled ? " record=1" : " record=0", f);
        fputs(" source_name=wavein", f);
        if (a->output_device[0])
            fprintf(f, " output_device_name=\"%s\"", a->output_device);
        if (a->input_device[0])
            fprintf(f, " input_device_name=\"%s\"", a->input_device);
        if (a->rate_sel > 0)
            fprintf(f, " rate=%d", audio_rate_values[a->rate_sel]);
        if (a->format_sel > 0)
            fprintf(f, " format=%s", audio_format_values[a->format_sel]);
        if (a->channels_sel > 0)
            fprintf(f, " channels=%d", audio_channels_values[a->channels_sel]);
        if (a->latency_sel > 0)
            fprintf(f, " fragments=%d fragment_size=%d",
                audio_latency_fragments[a->latency_sel],
                audio_latency_fragment_size[a->latency_sel]);
        fputs("\r\n", f);
    }
    if (a->null_sink_enabled)
        fputs("load-module module-null-sink sink_name=null\r\n", f);
    if (a->loopback_enabled)
        fputs("load-module module-loopback\r\n", f);

    {
        const char *port = a->pulseport[0] ? a->pulseport : "4713";
        const char *listens[2];
        int nlisten = 0;
        int i;

        if (a->listen_sel == 0)
            listens[nlisten++] = "127.0.0.1";
        else if (a->listen_sel == 1)
            listens[nlisten++] = "0.0.0.0";
        else {
            listens[nlisten++] = "0.0.0.0";
            listens[nlisten++] = "::";
        }

        for (i = 0; i < nlisten; i++) {
            fputs("load-module module-native-protocol-tcp", f);
            fprintf(f, " port=%s listen=%s", port, listens[i]);
            if (a->auth_sel == 1)
                fputs(" auth-anonymous=1", f);
            else if (a->auth_sel == 2 && a->auth_acl[0])
                fprintf(f, " auth-ip-acl=%s auth-cookie-enabled=0", a->auth_acl);
            else
                fprintf(f, " auth-cookie=%s", cookie_esc);
            fputs("\r\n", f);
        }
    }

    fclose(f);
}

/* Generate the PulseAudio auth cookie at %APPDATA%\Ming64X\pulse-cookie.  The
   daemon reads this exact file (see write_default_pa) and launchx ships the
   same bytes to the remote over SSH.  The cookie is PA_NATIVE_COOKIE_LENGTH
   (256) printable [A-Za-z0-9] bytes; it is regenerated if missing or short. */
static void
ensure_pulse_cookie(void)
{
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char dir[512], path[512];
    FILE *f;
    unsigned char cookie[256], raw[256];
    long sz;
    int i;

    if (config_dir(dir, sizeof dir))
        return;
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\pulse-cookie", dir);

    /* keep it only if a full-length cookie already exists */
    f = fopen(path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            sz = ftell(f);
            if (sz == (long)sizeof cookie) {
                fclose(f);
                return;
            }
        }
        fclose(f);
    }

    if (BCryptGenRandom(NULL, raw, sizeof raw,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return;
    for (i = 0; i < (int)sizeof cookie; i++)
        cookie[i] = (unsigned char)charset[raw[i] % (sizeof charset - 1)];

    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(cookie, 1, sizeof cookie, f);
    fclose(f);
}

/* Read the 256-byte PulseAudio cookie back.  Returns 1 on success. */
static int
read_pulse_cookie(unsigned char out[256])
{
    char dir[512], path[512];
    FILE *f;

    if (config_dir(dir, sizeof dir))
        return 0;
    snprintf(path, sizeof path, "%s\\pulse-cookie", dir);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(out, 1, 256, f) != 256) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

/* Set the working directory to the launcher's own folder, verify
   ming64x.exe is present there (else show an error with the full path),
   and start it with the given command line.  Returns 1 on success. */
static int
launch_ming64x(const char *cmdline)
{
    char dir[MAX_PATH];
    char full[MAX_PATH];
    char *slash;
    DWORD len;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    len = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return 0;
    slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';
    else {
        dir[0] = '.';
        dir[1] = '\0';
    }

    if (!SetCurrentDirectoryA(dir))
        return 0;

    snprintf(full, sizeof full, "%s\\ming64x.exe", dir);
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        char msg[640];
        snprintf(msg, sizeof msg,
            "Could not find ming64x.exe at:\n%s", full);
        MessageBoxA(NULL, msg, "LaunchX", MB_OK | MB_ICONERROR);
        return 0;
    }

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessA(NULL, (char *)cmdline, NULL, NULL, FALSE,
            0, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static int
validate_acl_inputs(struct options_network_and_access_control *net,
    struct options_audio *audio, char *err, int errsz)
{
    struct iprange_list *l;

    if (net->allow_string[0]) {
        l = iprange_parse(net->allow_string, NULL);
        if (!l) {
            snprintf(err, errsz,
                "\"Allowed IP addresses (-allow)\" contains an invalid entry:\n%s",
                net->allow_string);
            return 0;
        }
        iprange_free(l);
    }

    if (audio->auth_acl[0]) {
        l = iprange_parse(audio->auth_acl, NULL);
        if (!l) {
            snprintf(err, errsz,
                "\"Allowed clients\" (Audio) contains an invalid entry:\n%s",
                audio->auth_acl);
            return 0;
        }
        iprange_free(l);
    }

    return 1;
}

int main(int argc, char **argv)
{
    GdiFont *font;
    struct nk_context *ctx;
    WNDCLASSW wc = {0};
    RECT rect = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
    DWORD style = WS_OVERLAPPEDWINDOW;
    DWORD exstyle = WS_EX_APPWINDOW;
    HWND wnd;
    HDC dc;
    int running = 1;
    int needs_refresh = 1;
    int current_tab = 0;
    struct options_network_and_access_control net_opt;
    struct options_xdmcp xdmcp_opt;
    struct options_screen_windowing screen_opt;
    struct options_pointer_keyboard pointer_opt;
    struct options_xkb xkb_opt;
    struct options_accessx accessx_opt;
    struct options_desktop_integration desktop_opt;
    struct options_glx glx_opt;
    struct options_fonts_rendering fonts_opt;
    struct options_logging_extensions logging_opt;
    struct options_audio audio_opt;
    struct options_ssh_login ssh_opt;
    terminal term;
    ssh_session ssh;
    struct nk_image logo;

    /* Elevated helper mode: write the Hyper-V registry key and exit. */
    if (argc >= 3 && strcmp(argv[1], "--write-hyperv-reg") == 0) {
        struct options_network_and_access_control net;

        memset(&net, 0, sizeof(net));
        snprintf(net.vsockport, sizeof(net.vsockport), "%s", argv[2]);
        return hyperv_update_registry(&net);
    }

    reset_all_options(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt,
        &audio_opt);
    load_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt,
        &audio_opt);
    enumerate_audio_devices();
    ensure_pulse_cookie();   /* create at startup (no-op if it already exists) */
    if (sanitize_options(&xdmcp_opt, &screen_opt)) {
        strcpy(g_notice,
            "Your saved launchx.cnf contained conflicting options.\n"
            "They were reset to a valid combination. Review the\n"
            "settings before starting the server.");
    }
    if (ssh_opt.host[0] == '\0')
        g_focus_idx = 0;
    else if (ssh_opt.username[0] == '\0')
        g_focus_idx = 2;
    else if (ssh_opt.password[0] == '\0')
        g_focus_idx = 3;
    else
        g_focus_idx = 0;
    terminal_init(&term);
    ssh_session_init(&ssh);
    g_ssh = &ssh;
    g_current_tab = &current_tab;
    int row;
    DWORD last_time = 0;

    libssh2_init(0);

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_LAUNCHX));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"LaunchXWindowClass";
    RegisterClassW(&wc);

    AdjustWindowRectEx(&rect, style, FALSE, exstyle);
    wnd = CreateWindowExW(exstyle, wc.lpszClassName, L"LaunchX",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, wc.hInstance, NULL);
    dc = GetDC(wnd);
    SetTimer(wnd, 1, 16, NULL);

    font = nk_gdifont_create("Arial", 14);
    g_bold_font = nk_gdifont_create_bold("Arial", 14);
    g_bold_font->nk.userdata = nk_handle_ptr(g_bold_font);
    g_bold_font->nk.height = (float)g_bold_font->height;
    g_bold_font->nk.width = nk_gdifont_get_text_width;
    ctx = nk_gdi_init(font, dc, WINDOW_WIDTH, WINDOW_HEIGHT);
    logo = launchx_make_logo_image();

    while (running) {
        MSG msg;
        nk_input_begin(ctx);
        if (needs_refresh == 0) {
            if (GetMessageW(&msg, NULL, 0, 0) <= 0)
                running = 0;
            else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            needs_refresh = 1;
        } else {
            needs_refresh = 0;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            needs_refresh = 1;
        }
        nk_input_end(ctx);

        g_activate_pressed = 0;
        if (current_tab == 0 && !ssh_session_is_active(&ssh)) {
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER)) {
                g_activate_pressed = 1;
            } else {
                int i;
                for (i = 0; i < ctx->input.keyboard.text_len; ++i)
                    if (ctx->input.keyboard.text[i] == ' ') {
                        g_activate_pressed = 1;
                        break;
                    }
            }

            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TAB)) {
                int shift = ctx->input.keyboard.keys[NK_KEY_SHIFT].down;
                int n = g_focus_count;
                if (shift) {
                    if (g_focus_idx <= 0 || g_focus_idx >= n)
                        g_focus_idx = n - 1;
                    else
                        g_focus_idx--;
                } else {
                    if (g_focus_idx < 0 || g_focus_idx >= n - 1)
                        g_focus_idx = 0;
                    else
                        g_focus_idx++;
                }
                g_unfocus_edits = 1;
            }
        }

        if (ssh_session_is_active(&ssh) && current_tab == 0 &&
            !ssh_hostkey_pending(&ssh)) {
            if (ctx->input.keyboard.text_len > 0) {
                ssh_send(&ssh, ctx->input.keyboard.text,
                    (size_t)ctx->input.keyboard.text_len);
                ctx->input.keyboard.text_len = 0;
            }
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER))
                ssh_send(&ssh, "\r", 1);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_BACKSPACE))
                ssh_send(&ssh, "\x7f", 1);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_PASTE))
                ssh_paste_clipboard(&ssh);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TEXT_RESET_MODE))
                ssh_send(&ssh, "\x1b", 1);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP))
                ssh_send(&ssh, term.app_cursor_keys ? "\x1bOA" : "\x1b[A", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN))
                ssh_send(&ssh, term.app_cursor_keys ? "\x1bOB" : "\x1b[B", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_RIGHT))
                ssh_send(&ssh, term.app_cursor_keys ? "\x1bOC" : "\x1b[C", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_LEFT))
                ssh_send(&ssh, term.app_cursor_keys ? "\x1bOD" : "\x1b[D", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DEL))
                ssh_send(&ssh, "\x1b[3~", 4);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TEXT_START))
                ssh_send(&ssh, "\x1b[1~", 4);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_TEXT_END))
                ssh_send(&ssh, "\x1b[4~", 4);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP))
                ssh_send(&ssh, "\x1b[5~", 4);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN))
                ssh_send(&ssh, "\x1b[6~", 4);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F1))
                ssh_send(&ssh, "\x1bOP", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F2))
                ssh_send(&ssh, "\x1bOQ", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F3))
                ssh_send(&ssh, "\x1bOR", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F4))
                ssh_send(&ssh, "\x1bOS", 3);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F5))
                ssh_send(&ssh, "\x1b[15~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F6))
                ssh_send(&ssh, "\x1b[17~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F7))
                ssh_send(&ssh, "\x1b[18~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F8))
                ssh_send(&ssh, "\x1b[19~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F9))
                ssh_send(&ssh, "\x1b[20~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F10))
                ssh_send(&ssh, "\x1b[21~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F11))
                ssh_send(&ssh, "\x1b[23~", 5);
            if (nk_input_is_key_pressed(&ctx->input, NK_KEY_F12))
                ssh_send(&ssh, "\x1b[24~", 5);
        }
        ssh_pump(&ssh, &term);

        {
            DWORD now = GetTickCount();
            if (last_time == 0)
                last_time = now;
            ctx->delta_time_seconds = (float)(now - last_time) / 1000.0f;
            last_time = now;
        }

        {
            RECT client;
            struct nk_rect cr;
            float W, H;
            float logo_y = 0.0f;
            int prev_multiwindow, prev_xdmcp;

            /* Snapshot cross-tab state so we can tell which option changed */
            prev_multiwindow = screen_opt.multiwindow_enabled;
            prev_xdmcp = xdmcp_opt.xdmcp_enabled;

            g_field_menu_seen = 0;

            GetClientRect(wnd, &client);
            if (nk_begin(ctx, "LaunchX",
                nk_rect(0, 0, (float)client.right, (float)client.bottom),
                NK_WINDOW_NO_SCROLLBAR |
                ((g_confirm_reset || g_notice[0] || g_confirm_exit || g_acl_error[0] || g_hyperv_error[0]) ? (NK_WINDOW_ROM | NK_WINDOW_NO_INPUT) : 0)))
            {
                cr = nk_window_get_content_region(ctx);
                W = cr.w;
                H = cr.h;

                nk_layout_space_begin(ctx, NK_STATIC, H, 3);

                /* single column of 12 tabs, pinned to the left */
                nk_layout_space_push(ctx, nk_rect(0, 0, TAB_WIDTH, H - 45));
                if (nk_group_begin(ctx, "tabs", NK_WINDOW_NO_SCROLLBAR)) {
                    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0,0));
                    nk_style_push_float(ctx, &ctx->style.button.rounding, 0);
                    for (row = 0; row < 12; ++row) {
                        nk_layout_row_dynamic(ctx, 30, 1);
                        if (current_tab == row) {
                            struct nk_style_item normal = ctx->style.button.normal;
                            ctx->style.button.normal = ctx->style.button.active;
                            if (nk_button_label(ctx, tab_names[row]))
                                current_tab = row;
                            ctx->style.button.normal = normal;
                        } else if (nk_button_label(ctx, tab_names[row])) {
                            current_tab = row;
                        }
                    }
                    nk_style_pop_float(ctx);
                    nk_style_pop_vec2(ctx);
                    if (logo.handle.ptr) {
                        nk_layout_row_dynamic(ctx, 20, 1);
                        nk_spacing(ctx, 1);
                        nk_layout_row_dynamic(ctx, (float)X_LOGO_H, 1);
                        logo_y = nk_layout_widget_bounds(ctx).y;
                    }
                    nk_group_end(ctx);
                    if (logo.handle.ptr) {
                        nk_draw_image(nk_window_get_canvas(ctx),
                            nk_rect(cr.x, logo_y, (float)X_LOGO_W, (float)X_LOGO_H),
                            &logo, nk_rgb(255,255,255));
                    }
                }

                /* tab body: canvas flush with the top, to the right of tabs */
                g_focus_on = (current_tab == 0 && !ssh_session_is_active(&ssh));
                g_focus_seq = 0;
                if (!g_focus_on)
                    g_focus_idx = -1;
                nk_layout_space_push(ctx, nk_rect(TAB_WIDTH, 0, W - TAB_WIDTH, H - 45));
                if (nk_group_begin(ctx, tab_names[current_tab], NK_WINDOW_BORDER |
                        (current_tab == 0 ? NK_WINDOW_NO_SCROLLBAR : 0))) {
                    if (g_unfocus_edits) {
                        nk_edit_unfocus(ctx);
                        g_unfocus_edits = 0;
                    }
                    if (current_tab == 0) {
                        tab_ssh_login(ctx, &ssh_opt, &term, &ssh, &net_opt, &audio_opt);
                    } else if (current_tab == 1) {
                        tab_network_and_access_control(ctx, &net_opt);
                    } else if (current_tab == 2) {
                        tab_xdmcp(ctx, &xdmcp_opt);
                    } else if (current_tab == 3) {
                        tab_screen_and_windowing(ctx, &screen_opt);
                    } else if (current_tab == 4) {
                        tab_pointer_keyboard(ctx, &pointer_opt);
                    } else if (current_tab == 5) {
                        tab_xkb(ctx, &xkb_opt);
                    } else if (current_tab == 6) {
                        tab_accessx(ctx, &accessx_opt);
                    } else if (current_tab == 7) {
                        tab_desktop_integration(ctx, &desktop_opt);
                    } else if (current_tab == 8) {
                        tab_glx(ctx, &glx_opt);
                    } else if (current_tab == 9) {
                        tab_fonts_rendering(ctx, &fonts_opt);
                    } else if (current_tab == 10) {
                        tab_logging_extensions(ctx, &logging_opt);
                    } else if (current_tab == 11) {
                        tab_audio(ctx, &audio_opt);
                    }
                    nk_group_end(ctx);
                }

                /* Cross-tab conflict: XDMCP is invalid with -multiwindow.  Revert
                   whichever option was just enabled and explain why. */
                if (screen_opt.multiwindow_enabled && xdmcp_opt.xdmcp_enabled) {
                    if (screen_opt.multiwindow_enabled != prev_multiwindow) {
                        screen_opt.multiwindow_enabled = 0;
                        strcpy(g_notice,
                            "Cannot enable -multiwindow while XDMCP is enabled.\n"
                            "Disable XDMCP first.");
                    } else if (xdmcp_opt.xdmcp_enabled != prev_xdmcp) {
                        xdmcp_opt.xdmcp_enabled = 0;
                        strcpy(g_notice,
                            "Cannot enable XDMCP while -multiwindow mode is active.\n"
                            "Disable -multiwindow first.");
                    }
                }

                g_focus_count = g_focus_seq;

                /* OK / Exit, gravity South */
                nk_layout_space_push(ctx, nk_rect(0, H - 45, W, 45));
                if (nk_group_begin(ctx, "buttons", NK_WINDOW_NO_SCROLLBAR)) {
                    struct nk_rect bb;
                    nk_layout_row_dynamic(ctx, 30, 3);
                    bb = nk_widget_bounds(ctx);
                    if (nk_button_label(ctx, "Reset"))
                        g_confirm_reset = 1;
                    option_tooltip(ctx, bb, "Resets all configuration parameters in all sections to factory\ndefaults.");
                    if (nk_button_label(ctx, "Start")) {
                        if (!validate_acl_inputs(&net_opt, &audio_opt,
                                g_acl_error, sizeof(g_acl_error))) {
                            /* g_acl_error now holds the message; the modal
                               dialog below is shown this frame. */
                        }
                        else {
                            save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                                &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                                &fonts_opt, &logging_opt, &audio_opt);
                            {
                                struct cmdline c;
                                build_server_cmdline(&c, &ssh_opt, &ssh, &net_opt,
                                    &xdmcp_opt, &screen_opt, &pointer_opt,
                                    &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt,
                                    &logging_opt, &audio_opt);
                                write_commandline_file(c.buf);
                                if (audio_opt.audio_enabled) {
                                    write_default_pa(&audio_opt);
                                }
                                if (!net_opt.hyperv_registry_enabled ||
                                    hyperv_registry_apply(&net_opt) == 0)
                                    launch_ming64x(c.buf);
                            }
                        }
                    }
                    if (nk_button_label(ctx, "Exit")) {
                        if (ssh_x11_open_count(&ssh) > 0)
                            g_confirm_exit = 1;
                        else {
                            save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                                &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                                &fonts_opt, &logging_opt, &audio_opt);
                            running = 0;
                        }
                    }
                    nk_group_end(ctx);
                }

                nk_layout_space_end(ctx);
            }
        }
        nk_end(ctx);

        if (ssh_hostkey_pending(&ssh)) {
            RECT rc;
            struct nk_rect pr;
            char line1[512], line2[512];

            GetClientRect(wnd, &rc);
            pr = nk_rect((rc.right - 600.0f) / 2.0f,
                         (rc.bottom - 200.0f) / 2.0f, 600.0f, 200.0f);

            snprintf(line1, sizeof line1,
                "The authenticity of host '%s' can't be established.",
                ssh_hostkey_host(&ssh));
            snprintf(line2, sizeof line2,
                "%s key fingerprint is %s.",
                ssh_hostkey_keytype(&ssh), ssh_hostkey_fingerprint(&ssh));

            if (nk_begin(ctx, "Verify SSH host key", pr,
                    NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, line1, NK_TEXT_LEFT);
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, line2, NK_TEXT_LEFT);
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, "Are you sure you want to continue connecting?",
                    NK_TEXT_LEFT);
                nk_layout_row_dynamic(ctx, 34, 2);
                if (dialog_button(ctx, "Yes"))
                    ssh_hostkey_answer(&ssh, 1);
                if (dialog_button(ctx, "No"))
                    ssh_hostkey_answer(&ssh, 0);
            }
            nk_end(ctx);
        }

        if (g_confirm_reset) {
            RECT rc;
            struct nk_rect pr;
            const char *msg;

            GetClientRect(wnd, &rc);
            pr = nk_rect((rc.right - 640.0f) / 2.0f,
                         (rc.bottom - 120.0f) / 2.0f, 640.0f, 150.0f);

            msg = ssh_session_is_active(&ssh)
                ? "Do you want to reset all paramters to default and close your ssh session?"
                : "Do you want to reset all paramters to default?";

            if (nk_begin(ctx, "Confirm reset", pr,
                    NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                nk_layout_row_dynamic(ctx, 15, 1);
                nk_label_wrap(ctx, " ");
                nk_layout_row_dynamic(ctx, 35, 1);
                nk_label_wrap(ctx, msg);
                nk_layout_row_dynamic(ctx, 34, 2);
                if (dialog_button(ctx, "Yes")) {
                    if (ssh_session_is_active(&ssh))
                        ssh_session_stop(&ssh);
                    terminal_clear(&term);
                    reset_all_options(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
                        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt,
                        &audio_opt);
                    save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                        &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                        &fonts_opt, &logging_opt, &audio_opt);
                    g_confirm_reset = 0;
                }
                if (dialog_button(ctx, "No"))
                    g_confirm_reset = 0;
            }
            nk_end(ctx);
        }

        if (g_confirm_exit) {
            RECT rc;
            struct nk_rect pr;
            char msg[128];

            GetClientRect(wnd, &rc);
            pr = nk_rect((rc.right - 640.0f) / 2.0f,
                         (rc.bottom - 150.0f) / 2.0f, 640.0f, 190.0f);

            snprintf(msg, sizeof msg, "Exit and kill %d forwarded connections?",
                ssh_x11_open_count(&ssh));

            if (nk_begin(ctx, "Confirm exit", pr,
                    NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
                nk_layout_row_dynamic(ctx, 15, 1);
                nk_label_wrap(ctx, " ");
                nk_layout_row_dynamic(ctx, 55, 1);
                nk_label_wrap(ctx, msg);
                nk_layout_row_dynamic(ctx, 34, 2);
                if (dialog_button(ctx, "Confirm")) {
                    save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                        &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                        &fonts_opt, &logging_opt, &audio_opt);
                    running = 0;
                }
                if (dialog_button(ctx, "Cancel"))
                    g_confirm_exit = 0;
            }
            nk_end(ctx);
        }

        if (g_notice[0])
            ok_dialog(ctx, wnd, "Invalid option combination", g_notice, 190.0f, 55.0f);

        if (g_acl_error[0])
            ok_dialog(ctx, wnd, "Invalid IP list", g_acl_error, 190.0f, 55.0f);

        if (g_hyperv_error[0])
            ok_dialog(ctx, wnd, "Registry write failed", g_hyperv_error, 240.0f, 130.0f);

        if (g_field_menu_owner != NULL && !g_field_menu_seen)
            g_field_menu_owner = NULL;

        nk_gdi_render(nk_rgb(30, 30, 30));
    }

    save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt,
        &audio_opt);

    launchx_free_logo_image(&logo);
    nk_gdifont_del(g_bold_font);
    nk_gdifont_del(font);
    ReleaseDC(wnd, dc);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ssh_session_free(&ssh);
    terminal_free(&term);
    libssh2_exit();
    return 0;
}
