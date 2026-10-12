#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winreg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
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

#include "launchx_common.h"
#include "launchx_sshlogin.h"
#include "launchx_network.h"
#include "launchx_xdmcp.h"
#include "launchx_screen.h"
#include "launchx_pointer.h"
#include "launchx_xkb.h"
#include "launchx_accessx.h"
#include "launchx_desktop.h"
#include "launchx_glx.h"
#include "launchx_fonts.h"
#include "launchx_logging.h"
#include "launchx_audio.h"

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

static ssh_session *g_ssh;
static int *g_current_tab;

int g_focus_idx = -1;        /* focused widget in tab order, -1 = none */
static int g_focus_count = 0;       /* focusable widgets drawn last frame */
int g_focus_seq = 0;         /* running counter while drawing this frame */
static int g_focus_on = 0;          /* focus nav active (SSH login tab, not connected) */
static int g_unfocus_edits = 0;     /* Tab pressed: clear edit focus this frame */
static int g_activate_pressed = 0;  /* Enter/Space pressed: activate focused button */
int g_confirm_reset = 0;     /* Reset confirmation dialog is open (modal) */
static int g_confirm_exit = 0;      /* Exit-with-live-connections dialog is open (modal) */
static char g_notice[512];          /* cross-tab conflict explanation (modal) */
static char g_acl_error[512];       /* ACL validation error (modal) */
char g_hyperv_error[1024];   /* Hyper-V registry write error (modal) */

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

void
option_tooltip(struct nk_context *ctx, const struct nk_rect bounds, const char *text)
{
    static float timer = 0.0f;
    nk_do_tooltip_delay(ctx, text, bounds, &timer);
}

int
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

void
focus_ring(struct nk_context *ctx, const struct nk_rect b)
{
    nk_stroke_rect(nk_window_get_canvas(ctx),
        nk_rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2),
        0, 2.0f, nk_rgb(70, 140, 240));
}

int
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

void
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
void
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

nk_flags
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

nk_flags
text_option(struct nk_context *ctx, const char *label, char *buffer, const int buffer_size, const char *tooltip)
{
    nk_layout_row_dynamic(ctx, 30, 2);
    return text_option_cell(ctx, label, buffer, buffer_size, tooltip, NK_TEXT_LEFT);
}

nk_bool
readonly_filter(const struct nk_text_edit *box, nk_rune unicode)
{
    (void)box;
    (void)unicode;
    return 0;
}

void
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

void
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

static GdiFont *g_bold_font;

struct nk_user_font *
launchx_bold_font(void)
{
    return &g_bold_font->nk;
}

void
heading(struct nk_context *ctx, const char *text)
{
    nk_style_push_font(ctx, &g_bold_font->nk);
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, text, NK_TEXT_CENTERED);
    nk_style_pop_font(ctx);
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
        .byteswap_enabled = 1,
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
    e[n++] = CF_BOOL("networkingaccesscontrol", "allowdbigendianclients", net->byteswap_enabled);
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

int config_dir_real(char *out, size_t outsz)
{
    char dir[MAX_PATH], logical[MAX_PATH];
    HANDLE h;
    DWORD n;

    if (config_dir(dir, sizeof dir))
        return -1;
    snprintf(logical, sizeof logical, "%s\\test.txt", dir);

    h = CreateFileA(logical, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        n = GetFinalPathNameByHandleA(h, out, (DWORD) outsz, FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (n > 0 && n < outsz) {
            /* GetFinalPathNameByHandleA prefixes "\\?\" to a local DOS path */
            char *p = strrchr(out, '\\');
            if (p)
                *p = '\0';   /*  strip trailing  \test.txt   */
            if (!strncmp(out, "\\\\?\\", 4))  /* might have prefix    \\?\   */
                memmove(out, out + 4, strlen(out + 4) + 1);
            return 0;
        }
    }

    snprintf(out, outsz, "%s", dir);
    return 0;
}

int
config_dir(char *out, const size_t outsz)
{
    char appdata[MAX_PATH];

    /* SHGetFolderPathA is package-aware: for a Store/MSIX build it returns
       the virtualized roaming path (...\LocalCache\Roaming), matching where
       Windows actually redirects writes.  getenv("APPDATA") returns the
       un-virtualized C:\Users\...\AppData\Roaming, so the UI would display a
       directory that does not match the physical file location. */
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL,
                                SHGFP_TYPE_CURRENT, appdata)))
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
