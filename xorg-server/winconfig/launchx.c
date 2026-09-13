#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libssh2.h"

#include "../Nuklear/nukleardefault.h"

#include "terminal.h"
#include "ssh.h"

#include <math.h>

/* nuklear_gdi.h calls nk_cos/nk_sin, which are internal (static) nuklear
   helpers not exported by libnuklear.a; provide local wrappers. */
static float nk_cos(float x) { return cosf(x); }
static float nk_sin(float x) { return sinf(x); }

#define NK_GDI_IMPLEMENTATION
#include "../Nuklear/demo/gdi/nuklear_gdi.h"

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600
#define TAB_WIDTH     260

static const char *tab_names[11] = {
    "SSH login",
    "Networking & access control", "XDMCP", "Screen & windowing modes",
    "Pointer & keyboard input", "XKB keyboard layout", "AccessX key sequences",
    "Windows desktop integration", "OpenGL / GLX",
    "Fonts, rendering & appearance", "Logging, scheduling & extensions"
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

static ssh_session *g_ssh;
static int *g_current_tab;

static int g_focus_idx = -1;        /* focused widget in tab order, -1 = none */
static int g_focus_count = 0;       /* focusable widgets drawn last frame */
static int g_focus_seq = 0;         /* running counter while drawing this frame */
static int g_focus_on = 0;          /* focus nav active (SSH login tab, not connected) */
static int g_unfocus_edits = 0;     /* Tab pressed: clear edit focus this frame */
static int g_activate_pressed = 0;  /* Enter/Space pressed: activate focused button */
static int g_confirm_reset = 0;     /* Reset confirmation dialog is open (modal) */

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
};

static void
option_tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text)
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
focus_ring(struct nk_context *ctx, struct nk_rect b)
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

static void
checkbox_option(struct nk_context *ctx, const char *label, int *value, const char *tooltip)
{
    struct nk_rect b;
    nk_layout_row_dynamic(ctx, 30, 1);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, label, value);
    option_tooltip(ctx, b, tooltip);
}

static void
text_option(struct nk_context *ctx, const char *label, char *buffer, int buffer_size, const char *tooltip)
{
    struct nk_rect b;
    int focused;
    nk_flags ret;
    nk_layout_row_dynamic(ctx, 30, 2);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
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
}

static void
password_option(struct nk_context *ctx, const char *label, char *password,
    char *mask, int size, const char *tooltip)
{
    struct nk_rect b;
    int old_len = (int)strlen(mask);
    int new_len, i, s;
    int focused;
    nk_flags ret;
    char rebuilt[128];

    nk_layout_row_dynamic(ctx, 30, 2);
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
combobox_option(struct nk_context *ctx, const char *label, const char *const *items, int count, int *selected, const char *tooltip)
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
    int hex_size, const char *tooltip)
{
    struct nk_rect b;
    struct nk_colorf cf;
    struct nk_color col;
    char tmp[8];
    char old[8];
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

struct options_ssh_login {
    char host[128];
    char username[128];
    char password[128];
    char password_mask[128];
};

static void
tab_ssh_login(struct nk_context *ctx, struct options_ssh_login *opt,
    terminal *term, ssh_session *ssh,
    struct options_network_and_access_control *net)
{
    int cols_before = term->ncols;
    int rows_before = term->nrows;

    heading(ctx, "SSH login");
    text_option(ctx, "SSH Connect IP", opt->host, sizeof(opt->host),
        "Hostname or IP address of the SSH server to connect to");
    text_option(ctx, "Login username", opt->username, sizeof(opt->username),
        "Username to authenticate with on the SSH server");
    password_option(ctx, "Login password", opt->password, opt->password_mask,
        sizeof(opt->password),
        "Password to authenticate with on the SSH server");

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
        ssh_session_start(ssh, opt->host, opt->username, opt->password,
            net->listeningport_sel);
        ssh_request_resize(ssh, term->ncols, term->nrows);
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

    heading(ctx, "Networking & access control");

    combobox_option(ctx, "Listening port", listeningport_items, (int)LISTENINGPORT_COUNT, &opt->listeningport_sel, "Display number the server runs as. Clients connect on TCP port\n6000 plus this number (default :0).");

    checkbox_option(ctx, "Disable access control (-ac)", &opt->ac_enabled, "Disable host-based access control, so any host may connect and\nchange the access list. Use with caution.");

    if (opt->ac_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Allowed IP addresses (-allow)", opt->allow_string, (int)sizeof(opt->allow_string), "allow connections whose address matches a comma-separated IP list\n(e.g. 192.168.1.0/24,10.0.0.5-10.0.0.9,FE80::1)");
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
}

struct options_xdmcp {
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

    text_option(ctx, "Query host (-query)", opt->query_host, (int)sizeof(opt->query_host), "Enable XDMCP and send Query packets to this host.");

    checkbox_option(ctx, "Broadcast for XDMCP (-broadcast)", &opt->broadcast_enabled, "Enable XDMCP and broadcast a query to the network. The first\ndisplay manager to answer hosts the session.");

    text_option(ctx, "Indirect host (-indirect)", opt->indirect_host, (int)sizeof(opt->indirect_host), "Enable XDMCP and send IndirectQuery packets to this host.");

    checkbox_option(ctx, "IPv6 multicast (-multicast)", &opt->multicast_enabled, "Enable XDMCP and multicast a query to the network (IPv6 multicast).");

    text_option(ctx, "UDP port (-port)", opt->port_string, (int)sizeof(opt->port_string), "UDP port used for XDMCP packets (default 177).");

    text_option(ctx, "Local address (-from)", opt->from_address, (int)sizeof(opt->from_address), "Local address to connect from, useful when the machine has several\nnetwork interfaces.");

    checkbox_option(ctx, "Terminate after one session (-once)", &opt->once_enabled, "Exit the server when the XDMCP session ends, instead of resetting.");

    text_option(ctx, "Display class (-class)", opt->display_class, (int)sizeof(opt->display_class), "XDMCP display qualifier used when looking up display-specific\noptions (default MIT-unspecified).");

    text_option(ctx, "Magic cookie (-cookie)", opt->cookie, (int)sizeof(opt->cookie), "Private key shared with the display manager for XDM-AUTHORIZATION-1.");

    text_option(ctx, "Display ID (-displayID)", opt->display_id, (int)sizeof(opt->display_id), "Identifier the display manager uses to locate this display's\nshared key.");
}

struct options_screen_windowing {
    char screen_geometry[256];
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
};

static void
tab_screen_and_windowing(struct nk_context *ctx,
    struct options_screen_windowing *opt)
{
    heading(ctx, "Screen & windowing modes");

    text_option(ctx, "Screen geometry (-screen)", opt->screen_geometry, (int)sizeof(opt->screen_geometry), "Create screen <n> with optional size and position. Add @<monitor> to\nplace it on a monitor. Examples: 0 800x600+100+100@2 ; 0 @1");

    checkbox_option(ctx, "Run in fullscreen mode (-fullscreen)", &opt->fullscreen_enabled, "Make the X server window fill the entire Windows desktop.");

    checkbox_option(ctx, "Transparent root window (-rootless)", &opt->rootless_enabled, "Run rootless: the root window is hidden and only top-level X windows\nshow. Needs an external window manager; not with -multiwindow or\n-fullscreen.");

    checkbox_option(ctx, "Run in multiwindow mode (-multiwindow)", &opt->multiwindow_enabled, "Run multiwindow: each top-level X window becomes its own Windows\nwindow with a built-in window manager. Not with -rootless or\n-fullscreen.");

    checkbox_option(ctx, "No window border/titlebar (-nodecoration)", &opt->nodecoration_enabled, "Show the X window with no Windows border or title bar. Ignored when\n-fullscreen is set.");

    checkbox_option(ctx, "Use entire virtual screen (-multimonitors)", &opt->multimonitors_enabled, "Create one screen covering all monitors, with fake XINERAMA data\ndescribing each monitor.");

    combobox_option(ctx, "Resize mode (-resize)", resize_items, RESIZE_COUNT, &opt->resize_sel, "How the X screen resizes: scrollbars adds window scrollbars, randr\nuses the RANDR extension. Default is randr.");

    combobox_option(ctx, "Bit depth (-depth)", depth_items, DEPTH_COUNT, &opt->depth_sel, "Color depth in bits per pixel for fullscreen mode with a DirectDraw\nengine. Ignored without -fullscreen.");

    text_option(ctx, "Refresh rate (-refresh)", opt->refresh, (int)sizeof(opt->refresh), "Refresh rate (Hz) for fullscreen mode with a DirectDraw engine.\nIgnored without -fullscreen.");

    combobox_option(ctx, "Engine (-engine)", engine_items, ENGINE_COUNT, &opt->engine_sel, "Override the automatically selected drawing engine: 1 = Shadow GDI,\n4 = Shadow DirectDraw4 Non-Locking.");

    text_option(ctx, "Screen resolution DPI (-dpi)", opt->dpi, (int)sizeof(opt->dpi), "Screen resolution in dots per inch, for all screens.");

    combobox_option(ctx, "XINERAMA (+xinerama/-xinerama)", xinerama_items, XINERAMA_COUNT, &opt->xinerama_sel, "Enable (+) or disable (-) the XINERAMA extension.");

    checkbox_option(ctx, "Disable XINERAMA extension (-disablexineramaextension)", &opt->disablexinerama_enabled, "Disable the XINERAMA extension.");

    checkbox_option(ctx, "Hide Windows pointer (-lesspointer)", &opt->lesspointer_enabled, "Also hide the Windows pointer over inactive X windows, preventing a\nghost cursor. Only applies with -swcursor.");

    checkbox_option(ctx, "X11 software cursor (-swcursor)", &opt->swcursor_enabled, "Use the X11 software cursor instead of the Windows cursor.");
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
    char root_background[8];
    int retro_enabled;
    char color_visual_class[16];
    int nocursor_enabled;
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

    checkbox_option(ctx, "Disable cursor (-nocursor)", &opt->nocursor_enabled, "Disable the X cursor so it is not drawn on any screen.");
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
    char vmid[256];
    char vsockport[16];
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

    text_option(ctx, "Hyper-V VM GUID (-vmid)", opt->vmid, (int)sizeof(opt->vmid), "Hyper-V virtual machine GUID to accept VSock connections from.");

    text_option(ctx, "VSock listen port (-vsockport)", opt->vsockport, (int)sizeof(opt->vsockport), "Port number to listen on for VSock connections. Default 106000.");

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
    struct options_logging_extensions *logging_opt)
{
    *ssh_opt = (struct options_ssh_login) {0};

    *net_opt = (struct options_network_and_access_control) {
        .ac_enabled = 0,
        .allow_string = {0},
        .auth_file = {0},
        .byteswap_enabled = 0,
        .maxclients_sel = 4,      /* 1024 (LIMITCLIENTS) */
        .maxbigreqsize = 4,       /* 4 MB (MAX_BIG_REQUEST_SIZE) */
        .listeningport_sel = 0,   /* :0 (default) */
    };
    *xdmcp_opt = (struct options_xdmcp) {
        .query_host = {0},
        .broadcast_enabled = 0,
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
        .screen_geometry = {0},
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
        .nocursor_enabled = 0,
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
        .vmid = {0},
        .vsockport = "106000",
        .extension_enabled = {
            1, 1, 1, 0,  /* SHAPE, XTEST, SECURITY, XINERAMA(off by default) */
            1, 1, 1, 1,  /* XFIXES, XFree86-Bigfont, RENDER, RANDR */
            1, 1, 1, 1,  /* COMPOSITE, DAMAGE, MIT-SCREEN-SAVER, DOUBLE-BUFFER */
            1, 1, 1, 1,  /* RECORD, DPMS, X-Resource, GLX */
        },
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
    struct options_logging_extensions *logging)
{
    int n = 0, i;

    e[n++] = CF_STR("sshlogin", "sshconnectip", ssh->host);
    e[n++] = CF_STR("sshlogin", "loginusername", ssh->username);
    /* login password is intentionally not persisted */

    e[n++] = CF_BOOL("networkingaccesscontrol", "disableaccesscontrol", net->ac_enabled);
    e[n++] = CF_STR("networkingaccesscontrol", "allowedipaddresses", net->allow_string);
    e[n++] = CF_STR("networkingaccesscontrol", "authorizationfile", net->auth_file);
    e[n++] = CF_BOOL("networkingaccesscontrol", "allowdifferentendiannessclients", net->byteswap_enabled);
    e[n++] = CF_INT("networkingaccesscontrol", "maxclients", net->maxclients_sel, 0, (int)MAXCLIENTS_COUNT - 1);
    e[n++] = CF_INT("networkingaccesscontrol", "maxbigreqsize", net->maxbigreqsize, 1, 127);
    e[n++] = CF_INT("networkingaccesscontrol", "listeningport", net->listeningport_sel, 0, (int)LISTENINGPORT_COUNT - 1);

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

    e[n++] = CF_STR("screenwindowingmodes", "screengeometry", screen->screen_geometry);
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
    e[n++] = CF_BOOL("fontsrenderingappearance", "disablecursor", fonts->nocursor_enabled);

    e[n++] = CF_STR("loggingschedulingextensions", "logfile", logging->logfile);
    e[n++] = CF_INT("loggingschedulingextensions", "logverbosity", logging->logverbose, 0, 3);
    e[n++] = CF_BOOL("loggingschedulingextensions", "abortonfatalerror", logging->core_enabled);
    e[n++] = CF_STR("loggingschedulingextensions", "audittraillevel", logging->audit);
    e[n++] = CF_BOOL("loggingschedulingextensions", "disablesmartscheduling", logging->dumbSched_enabled);
    e[n++] = CF_STR("loggingschedulingextensions", "schedulerinterval", logging->schedInterval);
    e[n++] = CF_STR("loggingschedulingextensions", "schedulermaxslice", logging->schedMax);
    e[n++] = CF_BOOL("loggingschedulingextensions", "disabletestingextensions", logging->tst_enabled);
    e[n++] = CF_STR("loggingschedulingextensions", "hypervvmguid", logging->vmid);
    e[n++] = CF_STR("loggingschedulingextensions", "vsocklistenport", logging->vsockport);
    for (i = 0; i < NUM_EXTENSIONS; ++i)
        e[n++] = CF_BOOL("loggingschedulingextensions", extension_keys[i], logging->extension_enabled[i]);

    return n;
}

static int
config_dir(char *out, size_t outsz)
{
    const char *appdata = getenv("APPDATA");
    if (!appdata || !appdata[0])
        return -1;
    snprintf(out, outsz, "%s\\Ming64X", appdata);
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
    struct options_logging_extensions *logging)
{
    struct cfentry e[160];
    int n = cf_build(e, ssh, net, xdmcp, screen, pointer, xkb, accessx, desktop, glx, fonts, logging);
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
    struct options_logging_extensions *logging)
{
    struct cfentry e[160];
    int n = cf_build(e, ssh, net, xdmcp, screen, pointer, xkb, accessx, desktop, glx, fonts, logging);
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
cl_if(struct cmdline *c, int cond, const char *flag)
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
    struct options_logging_extensions *logging)
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

    /* XDMCP */
    cl_opt(c, "-query", xdmcp->query_host, NULL);
    cl_if(c, xdmcp->broadcast_enabled, "-broadcast");
    cl_opt(c, "-indirect", xdmcp->indirect_host, NULL);
    cl_if(c, xdmcp->multicast_enabled, "-multicast");
    cl_opt(c, "-port", xdmcp->port_string, "177");
    cl_opt(c, "-from", xdmcp->from_address, NULL);
    cl_if(c, xdmcp->once_enabled, "-once");
    cl_opt(c, "-class", xdmcp->display_class, "MIT-unspecified");
    cl_opt(c, "-cookie", xdmcp->cookie, NULL);
    cl_opt(c, "-displayID", xdmcp->display_id, NULL);

    /* Screen & windowing modes */
    if (screen->screen_geometry[0]) {
        char geo[256];
        char *p;
        strcpy(geo, screen->screen_geometry);
        cl_arg(c, "-screen");
        p = geo;
        while (*p) {
            char *s, save;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            s = p;
            while (*p && *p != ' ' && *p != '\t')
                ++p;
            save = *p;
            *p = '\0';
            cl_arg(c, s);
            *p = save;
            if (save)
                ++p;
        }
    }
    cl_if(c, screen->fullscreen_enabled, "-fullscreen");
    cl_if(c, screen->rootless_enabled, "-rootless");
    cl_if(c, screen->multiwindow_enabled, "-multiwindow");
    cl_if(c, screen->nodecoration_enabled, "-nodecoration");
    cl_if(c, screen->multimonitors_enabled, "-multimonitors");
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
    cl_if(c, fonts->nocursor_enabled, "-nocursor");

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
    cl_opt(c, "-vmid", logging->vmid, NULL);
    cl_opt(c, "-vsockport", logging->vsockport, "106000");
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

int main(void)
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
    struct options_ssh_login ssh_opt;
    terminal term;
    ssh_session ssh;

    reset_all_options(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);
    load_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);
    if (ssh_opt.host[0] == '\0')
        g_focus_idx = 0;
    else if (ssh_opt.username[0] == '\0')
        g_focus_idx = 1;
    else if (ssh_opt.password[0] == '\0')
        g_focus_idx = 2;
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
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
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

            GetClientRect(wnd, &client);
            if (nk_begin(ctx, "LaunchX",
                nk_rect(0, 0, (float)client.right, (float)client.bottom),
                NK_WINDOW_NO_SCROLLBAR |
                (g_confirm_reset ? (NK_WINDOW_ROM | NK_WINDOW_NO_INPUT) : 0)))
            {
                cr = nk_window_get_content_region(ctx);
                W = cr.w;
                H = cr.h;

                nk_layout_space_begin(ctx, NK_STATIC, H, 3);

                /* single column of 11 tabs, pinned to the left */
                nk_layout_space_push(ctx, nk_rect(0, 0, TAB_WIDTH, H - 45));
                if (nk_group_begin(ctx, "tabs", NK_WINDOW_NO_SCROLLBAR)) {
                    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0,0));
                    nk_style_push_float(ctx, &ctx->style.button.rounding, 0);
                    for (row = 0; row < 11; ++row) {
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
                    nk_group_end(ctx);
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
                        tab_ssh_login(ctx, &ssh_opt, &term, &ssh, &net_opt);
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
                    }
                    nk_group_end(ctx);
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
                        save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                            &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                            &fonts_opt, &logging_opt);
                        {
                            struct cmdline c;
                            build_server_cmdline(&c, &ssh_opt, &ssh, &net_opt,
                                &xdmcp_opt, &screen_opt, &pointer_opt,
                                &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt,
                                &logging_opt);
                            write_commandline_file(c.buf);
                            launch_ming64x(c.buf);
                        }
                    }
                    if (nk_button_label(ctx, "Exit")) {
                        save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                            &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                            &fonts_opt, &logging_opt);
                        running = 0;
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
                if (nk_button_label(ctx, "Yes"))
                    ssh_hostkey_answer(&ssh, 1);
                if (nk_button_label(ctx, "No"))
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
                {
                    struct nk_rect b = nk_widget_bounds(ctx);
                    nk_button_label(ctx, "Yes");
                    if (nk_input_has_mouse_click_in_button_rect(&ctx->input, NK_BUTTON_LEFT, b) &&
                        nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT)) {
                        if (ssh_session_is_active(&ssh))
                            ssh_session_stop(&ssh);
                        terminal_clear(&term);
                        reset_all_options(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
                            &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);
                        save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt,
                            &pointer_opt, &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt,
                            &fonts_opt, &logging_opt);
                        g_confirm_reset = 0;
                    }
                }
                {
                    struct nk_rect b = nk_widget_bounds(ctx);
                    nk_button_label(ctx, "No");
                    if (nk_input_has_mouse_click_in_button_rect(&ctx->input, NK_BUTTON_LEFT, b) &&
                        nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT))
                        g_confirm_reset = 0;
                }
            }
            nk_end(ctx);
        }

        nk_gdi_render(nk_rgb(30, 30, 30));
    }

    save_config(&ssh_opt, &net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &accessx_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);

    nk_gdifont_del(g_bold_font);
    nk_gdifont_del(font);
    ReleaseDC(wnd, dc);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ssh_session_free(&ssh);
    terminal_free(&term);
    libssh2_exit();
    return 0;
}
