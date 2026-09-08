#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "../Nuklear/nukleardefault.h"

#include <math.h>

/* nuklear_gdi.h calls nk_cos/nk_sin, which are internal (static) nuklear
   helpers not exported by libnuklear.a; provide local wrappers. */
static float nk_cos(float x) { return cosf(x); }
static float nk_sin(float x) { return sinf(x); }

#define NK_GDI_IMPLEMENTATION
#include "../Nuklear/demo/gdi/nuklear_gdi.h"

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 700

static const char *tab_names[9] = {
    "Networking & access control", "XDMCP", "Screen & windowing modes",
    "Pointer & keyboard input", "XKB keyboard layout", "Windows desktop integration",
    "OpenGL / GLX", "Fonts, rendering & appearance", "Logging, scheduling & extensions"
};

static const char *maxclients_items[] = {"64", "128", "256", "512", "1024", "2048"};

#define MAXCLIENTS_COUNT (sizeof(maxclients_items) / sizeof(maxclients_items[0]))

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

static const char *rootbackground_items[] = {"Black (default)", "White (-wr)", "None (-background none)"};
#define ROOTBACKGROUND_COUNT (sizeof(rootbackground_items) / sizeof(rootbackground_items[0]))

#define NUM_EXTENSIONS 16
static const char *extension_names[NUM_EXTENSIONS] = {
    "SHAPE", "XTEST", "SECURITY", "XINERAMA", "XFIXES",
    "XFree86-Bigfont", "RENDER", "RANDR", "COMPOSITE", "DAMAGE",
    "MIT-SCREEN-SAVER", "DOUBLE-BUFFER", "RECORD", "DPMS",
    "X-Resource", "GLX"
};

static LRESULT CALLBACK
WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
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
};

static void
option_tooltip(struct nk_context *ctx, struct nk_rect bounds, const char *text)
{
    static float timer = 0.0f;
    nk_do_tooltip_delay(ctx, text, bounds, &timer);
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
    nk_layout_row_dynamic(ctx, 30, 2);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    option_tooltip(ctx, b, tooltip);
    b = nk_widget_bounds(ctx);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buffer, buffer_size, nk_filter_default);
    option_tooltip(ctx, b, tooltip);
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

static void
tab_network_and_access_control(struct nk_context *ctx,
    struct options_network_and_access_control *opt)
{
    struct nk_rect b;

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Networking & access control", NK_TEXT_CENTERED);

    checkbox_option(ctx, "Disable access control (-ac)", &opt->ac_enabled, "disable access control restrictions");

    if (opt->ac_enabled)
        nk_widget_disable_begin(ctx);
    text_option(ctx, "Allowed IP addresses (-allow)", opt->allow_string, (int)sizeof(opt->allow_string), "allow connections whose address matches ALLOWSTRING (e.g. 192.168.1.0/24,10.0.0.5-10.0.0.9,FE80::1)");
    if (opt->ac_enabled)
        nk_widget_disable_end(ctx);

    text_option(ctx, "Authorization file (-auth)", opt->auth_file, (int)sizeof(opt->auth_file), "select authorization file");

    checkbox_option(ctx, "Allow different-endianness clients (+byteswappedclients)", &opt->byteswap_enabled, "Allow clients with endianess different to that of the server");

    combobox_option(ctx, "Max clients (-maxclients)", maxclients_items, (int)MAXCLIENTS_COUNT, &opt->maxclients_sel, "set maximum number of clients (power of two)");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Max bigreq size (MB)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Set maximum big request size to size MB");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 1, &opt->maxbigreqsize, 127, 1);
    option_tooltip(ctx, b, "Set maximum big request size to size MB");
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
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "XDMCP", NK_TEXT_CENTERED);

    text_option(ctx, "Query host (-query)", opt->query_host, (int)sizeof(opt->query_host), "contact named host for XDMCP");

    checkbox_option(ctx, "Broadcast for XDMCP (-broadcast)", &opt->broadcast_enabled, "broadcast for XDMCP");

    text_option(ctx, "Indirect host (-indirect)", opt->indirect_host, (int)sizeof(opt->indirect_host), "contact named host for indirect XDMCP");

    checkbox_option(ctx, "IPv6 multicast (-multicast)", &opt->multicast_enabled, "IPv6 multicast for XDMCP");

    text_option(ctx, "UDP port (-port)", opt->port_string, (int)sizeof(opt->port_string), "UDP port number to send messages to");

    text_option(ctx, "Local address (-from)", opt->from_address, (int)sizeof(opt->from_address), "specify the local address to connect from");

    checkbox_option(ctx, "Terminate after one session (-once)", &opt->once_enabled, "Terminate server after one session");

    text_option(ctx, "Display class (-class)", opt->display_class, (int)sizeof(opt->display_class), "specify display class to send in manage");

    text_option(ctx, "Magic cookie (-cookie)", opt->cookie, (int)sizeof(opt->cookie), "specify the magic cookie for XDMCP");

    text_option(ctx, "Display ID (-displayID)", opt->display_id, (int)sizeof(opt->display_id), "manufacturer display ID for request");
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
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Screen & windowing modes", NK_TEXT_CENTERED);

    text_option(ctx, "Screen geometry (-screen)", opt->screen_geometry, (int)sizeof(opt->screen_geometry), "enable screen scr_num and optionally specify a width and height and initial position for that screen; a monitor number can be specified to start the server on. Examples: 0 800x600+100+100@2 ; 0 1024x768@3 ; 0 @1");

    checkbox_option(ctx, "Run in fullscreen mode (-fullscreen)", &opt->fullscreen_enabled, "Run the server in fullscreen mode.");

    checkbox_option(ctx, "Transparent root window (-rootless)", &opt->rootless_enabled, "Use a transparent root window with an external window manager (such as openbox). Not to be used with -multiwindow or with -fullscreen.");

    checkbox_option(ctx, "Run in multiwindow mode (-multiwindow)", &opt->multiwindow_enabled, "Run the server in multiwindow mode. Not to be used together with -rootless or -fullscreen.");

    checkbox_option(ctx, "No window border/titlebar (-nodecoration)", &opt->nodecoration_enabled, "Do not draw a window border, title bar, etc. Windowed mode only i.e. ignored when -fullscreen specified.");

    checkbox_option(ctx, "Use entire virtual screen (-multimonitors)", &opt->multimonitors_enabled, "Use the entire virtual screen if multiple monitors are present.");

    combobox_option(ctx, "Resize mode (-resize)", resize_items, RESIZE_COUNT, &opt->resize_sel, "In windowed mode, set the resizing mode. 'scrollbars' mode gives the window scrollbars as needed, 'randr' mode uses the RANDR extension to resize the X screen. 'randr' is the default.");

    combobox_option(ctx, "Bit depth (-depth)", depth_items, DEPTH_COUNT, &opt->depth_sel, "Specify an optional bitdepth to use in fullscreen mode with a DirectDraw engine.");

    text_option(ctx, "Refresh rate (-refresh)", opt->refresh, (int)sizeof(opt->refresh), "Specify an optional refresh rate to use in fullscreen mode with a DirectDraw engine.");

    combobox_option(ctx, "Engine (-engine)", engine_items, ENGINE_COUNT, &opt->engine_sel, "Override the server's automatically selected engine type: 1 - Shadow GDI, 4 - Shadow DirectDraw4 Non-Locking");

    text_option(ctx, "Screen resolution DPI (-dpi)", opt->dpi, (int)sizeof(opt->dpi), "screen resolution in dots per inch");

    combobox_option(ctx, "XINERAMA (+xinerama/-xinerama)", xinerama_items, XINERAMA_COUNT, &opt->xinerama_sel, "+xinerama enables the XINERAMA extension, -xinerama disables it.");

    checkbox_option(ctx, "Disable XINERAMA extension (-disablexineramaextension)", &opt->disablexinerama_enabled, "Disable the XINERAMA extension (hack)");

    checkbox_option(ctx, "Hide Windows pointer (-lesspointer)", &opt->lesspointer_enabled, "Hide the windows mouse pointer when it is over any XWin window. This prevents ghost cursors appearing when the Windows cursor is drawn on top of the X cursor");

    checkbox_option(ctx, "X11 software cursor (-swcursor)", &opt->swcursor_enabled, "Disable the usage of the Windows cursor and use the X11 software cursor instead.");
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
    int accessx_enabled;
    char autorepeat_delay[16];
    char autorepeat_interval[16];
};

static void
tab_pointer_keyboard(struct nk_context *ctx, struct options_pointer_keyboard *opt)
{
    struct nk_rect b;

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Pointer & keyboard input", NK_TEXT_CENTERED);

    checkbox_option(ctx, "Emulate 3-button mouse (-emulate3buttons)", &opt->emulate3buttons_enabled, "Emulate 3 button mouse with an optional timeout in milliseconds.");

    text_option(ctx, "Emulate timeout (ms)", opt->emulate3buttons_timeout, (int)sizeof(opt->emulate3buttons_timeout), "Emulate 3 button mouse with an optional timeout in milliseconds.");

    checkbox_option(ctx, "Alt+F4 exits server (-winkill)", &opt->winkill_enabled, "Alt+F4 exits the X Server.");

    checkbox_option(ctx, "Ctrl+Alt+Backspace exits server (-unixkill)", &opt->unixkill_enabled, "Ctrl-Alt-Backspace exits the X Server. The Ctrl-Alt-Backspace key combo is disabled by default.");

    checkbox_option(ctx, "Grab special Windows keys (-keyhook)", &opt->keyhook_enabled, "Grab special Windows keypresses like Alt-Tab or the Menu key.");

    checkbox_option(ctx, "Ignore keyboard and mouse input (-ignoreinput)", &opt->ignoreinput_enabled, "Ignore keyboard and mouse input.");

    checkbox_option(ctx, "Enable auto-repeat (r)", &opt->autorepeat_enabled, "-r turns off auto-repeat, r turns on auto-repeat");

    text_option(ctx, "Pointer acceleration (-a)", opt->pointer_acceleration, (int)sizeof(opt->pointer_acceleration), "default pointer acceleration (factor)");

    text_option(ctx, "Pointer threshold (-t)", opt->pointer_threshold, (int)sizeof(opt->pointer_threshold), "default pointer threshold (pixels/t)");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Bell base (-f)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "bell base (0-100)");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 0, &opt->bell, 100, 1);
    option_tooltip(ctx, b, "bell base (0-100)");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->bell);

    checkbox_option(ctx, "Enable AccessX key sequences (+accessx)", &opt->accessx_enabled, "[+-]accessx [ timeout [ timeout_mask [ feedback [ options_mask] ] ] ] enable/disable accessx key sequences");

    text_option(ctx, "Auto-repeat delay (-ardelay)", opt->autorepeat_delay, (int)sizeof(opt->autorepeat_delay), "set XKB autorepeat delay");

    text_option(ctx, "Auto-repeat interval (-arinterval)", opt->autorepeat_interval, (int)sizeof(opt->autorepeat_interval), "set XKB autorepeat interval");
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
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "XKB keyboard layout", NK_TEXT_CENTERED);

    text_option(ctx, "Layout (-xkblayout)", opt->xkblayout, (int)sizeof(opt->xkblayout), "Set the layout to use for XKB. This defaults to a layout matching your current layout from Windows or us (i.e. USA) if no matching layout was found. For example: -xkblayout de");

    text_option(ctx, "Model (-xkbmodel)", opt->xkbmodel, (int)sizeof(opt->xkbmodel), "Set the model to use for XKB. This defaults to pc105.");

    text_option(ctx, "Variant (-xkbvariant)", opt->xkbvariant, (int)sizeof(opt->xkbvariant), "Set the variant to use for XKB. This defaults to not set. For example: -xkbvariant nodeadkeys");

    text_option(ctx, "Options (-xkboptions)", opt->xkboptions, (int)sizeof(opt->xkboptions), "Set the options to use for XKB. This defaults to not set.");

    text_option(ctx, "Rules (-xkbrules)", opt->xkbrules, (int)sizeof(opt->xkbrules), "Set the rules to use for XKB. This defaults to xorg.");

    text_option(ctx, "Base directory (-xkbdir)", opt->xkbdir, (int)sizeof(opt->xkbdir), "base directory for XKB configuration files");
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
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Windows desktop integration", NK_TEXT_CENTERED);

    checkbox_option(ctx, "Clipboard integration (-clipboard)", &opt->clipboard_enabled, "Enable [disable] the clipboard integration. Default is enabled.");

    checkbox_option(ctx, "Map PRIMARY selection to clipboard (-primary)", &opt->primary_enabled, "When clipboard integration is enabled, map the X11 PRIMARY selection to the Windows clipboard. The CLIPBOARD selection is always mapped if -clipboard is enabled. Default is enabled.");

    checkbox_option(ctx, "Convert ANSI code page (-codepage)", &opt->codepage_enabled, "Convert Windows ANSI code page text for legacy 8-bit clients");

    checkbox_option(ctx, "Add host names to window titles (-hostintitle)", &opt->hostintitle_enabled, "In multiwindow mode, add remote host names to window titles.");

    checkbox_option(ctx, "Notification-area icon (-trayicon)", &opt->trayicon_enabled, "Do not create a notification area icon. Default is to create one icon per screen. You can globally disable notification area icons with -notrayicon, then enable them for specific screens with -trayicon for those screens.");

    text_option(ctx, "Window icon (-icon)", opt->icon_spec, (int)sizeof(opt->icon_spec), "Set screen window icon in windowed mode.");

    checkbox_option(ctx, "Composite extension (-compositewm)", &opt->compositewm_enabled, "Enable [Disable] Composite extension. Default is enabled. Used in -multiwindow mode. Use Composite extension redirection to maintain a bitmap image of each top-level X window, so window contents which are occluded show correctly in Taskbar and Task Switcher previews.");

    checkbox_option(ctx, "Composite per-pixel alpha (-compositealpha)", &opt->compositealpha_enabled, "X windows with per-pixel alpha are composited into the Windows desktop.");

    text_option(ctx, "Clip update boxes (-clipupdates)", opt->clipupdates, (int)sizeof(opt->clipupdates), "Use a clipping region to constrain shadow update blits to the updated region when num_boxes, or more, are in the updated region. Diminished effect on current Windows versions because they already group GDI operations together in a batch, which has a similar effect.");
}

struct options_glx {
    int wgl_enabled;
    int swrastwgl_enabled;
    int iglx_enabled;
};

static void
tab_glx(struct nk_context *ctx, struct options_glx *opt)
{
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "OpenGL / GLX", NK_TEXT_CENTERED);

    checkbox_option(ctx, "Native WGL for GLX (-wgl)", &opt->wgl_enabled, "Enable the GLX extension to use the native Windows WGL interface for hardware-accelerated OpenGL");

    checkbox_option(ctx, "WGL swrast for GLX (-swrastwgl)", &opt->swrastwgl_enabled, "Enable the GLX extension to use the native Windows WGL interface based on the swrast interface for accelerated OpenGL");

    checkbox_option(ctx, "Allow indirect GLX contexts (+iglx)", &opt->iglx_enabled, "+iglx Allow creating indirect GLX contexts (default), -iglx Prohibit creating indirect GLX contexts");
}

struct options_fonts_rendering {
    char font_path[1024];
    int render_sel;
    int deferglyphs_sel;
    char fakescreenfps[16];
    int backingstore_sel;
    int rootbackground_sel;
    int retro_enabled;
    char color_visual_class[16];
    int nocursor_enabled;
};

static void
tab_fonts_rendering(struct nk_context *ctx, struct options_fonts_rendering *opt)
{
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Fonts, rendering & appearance", NK_TEXT_CENTERED);

    text_option(ctx, "Font path (-fp)", opt->font_path, (int)sizeof(opt->font_path), "default font path");

    combobox_option(ctx, "Render policy (-render)", render_items, RENDER_COUNT, &opt->render_sel, "set render color alloc policy");

    combobox_option(ctx, "Defer glyphs (-deferglyphs)", deferglyphs_items, DEFERGLYPHS_COUNT, &opt->deferglyphs_sel, "defer loading of [no|all|16-bit] glyphs");

    text_option(ctx, "Fake screen fps (-fakescreenfps)", opt->fakescreenfps, (int)sizeof(opt->fakescreenfps), "fake screen default fps (1-600)");

    combobox_option(ctx, "Backing store (+bs/-bs)", backingstore_items, BACKINGSTORE_COUNT, &opt->backingstore_sel, "+bs enable any backing store support / -bs disable any backing store support");

    combobox_option(ctx, "Root background", rootbackground_items, ROOTBACKGROUND_COUNT, &opt->rootbackground_sel, "-br create root window with black background / -wr create root window with white background / -background [none] create root window with no background");

    checkbox_option(ctx, "Start with classic stipple (-retro)", &opt->retro_enabled, "start with classic stipple");

    text_option(ctx, "Color visual class (-cc)", opt->color_visual_class, (int)sizeof(opt->color_visual_class), "default color visual class");

    checkbox_option(ctx, "Disable cursor (-nocursor)", &opt->nocursor_enabled, "disable the cursor");
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

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Logging, scheduling & extensions", NK_TEXT_CENTERED);

    text_option(ctx, "Log file (-logfile)", opt->logfile, (int)sizeof(opt->logfile), "Write log messages to <filename>.");

    nk_layout_row_dynamic(ctx, 30, 3);
    b = nk_widget_bounds(ctx);
    nk_label(ctx, "Log verbosity (-logverbose)", NK_TEXT_LEFT);
    option_tooltip(ctx, b, "Set the verbosity of log messages. 0 - only print fatal error. 1 - print additional configuration information. 2 - print additional runtime information [default]. 3 - print debugging and tracing information.");
    b = nk_widget_bounds(ctx);
    nk_slider_int(ctx, 0, &opt->logverbose, 3, 1);
    option_tooltip(ctx, b, "Set the verbosity of log messages. 0 - only print fatal error. 1 - print additional configuration information. 2 - print additional runtime information [default]. 3 - print debugging and tracing information.");
    nk_labelf(ctx, NK_TEXT_LEFT, "%d", opt->logverbose);

    checkbox_option(ctx, "Abort on fatal error (-core)", &opt->core_enabled, "abort on fatal error (may produce a crash dump)");

    text_option(ctx, "Audit trail level (-audit)", opt->audit, (int)sizeof(opt->audit), "set audit trail level");

    checkbox_option(ctx, "Disable smart scheduling (-dumbSched)", &opt->dumbSched_enabled, "Disable smart scheduling and threaded input, enable old behavior");

    text_option(ctx, "Scheduler interval (-schedInterval)", opt->schedInterval, (int)sizeof(opt->schedInterval), "Set scheduler interval in msec");

    text_option(ctx, "Scheduler max slice (-schedMax)", opt->schedMax, (int)sizeof(opt->schedMax), "Set scheduler max slice (msec)");

    checkbox_option(ctx, "Disable testing extensions (-tst)", &opt->tst_enabled, "disable testing extensions");

    text_option(ctx, "Hyper-V VM GUID (-vmid)", opt->vmid, (int)sizeof(opt->vmid), "Hyper-V VM GUID to accept VSock connections from");

    text_option(ctx, "VSock listen port (-vsockport)", opt->vsockport, (int)sizeof(opt->vsockport), "integer port number to listen for VSock connections. Default 106000.");

    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, "Extensions", NK_TEXT_LEFT);
    for (i = 0; i < NUM_EXTENSIONS; ++i) {
        char tip[128];
        snprintf(tip, sizeof(tip),
            "+extension %s enables, -extension %s disables",
            extension_names[i], extension_names[i]);
        checkbox_option(ctx, extension_names[i], &opt->extension_enabled[i], tip);
    }
}

static void
reset_all_options(struct options_network_and_access_control *net_opt,
    struct options_xdmcp *xdmcp_opt,
    struct options_screen_windowing *screen_opt,
    struct options_pointer_keyboard *pointer_opt,
    struct options_xkb *xkb_opt,
    struct options_desktop_integration *desktop_opt,
    struct options_glx *glx_opt,
    struct options_fonts_rendering *fonts_opt,
    struct options_logging_extensions *logging_opt)
{
    *net_opt = (struct options_network_and_access_control) {
        .ac_enabled = 0,
        .allow_string = {0},
        .auth_file = {0},
        .byteswap_enabled = 0,
        .maxclients_sel = 4,      /* 1024 (LIMITCLIENTS) */
        .maxbigreqsize = 4,       /* 4 MB (MAX_BIG_REQUEST_SIZE) */
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
        .accessx_enabled = 0,
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
        .rootbackground_sel = 0,  /* Black (default) */
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
    char option[9][128];
    struct options_network_and_access_control net_opt;
    struct options_xdmcp xdmcp_opt;
    struct options_screen_windowing screen_opt;
    struct options_pointer_keyboard pointer_opt;
    struct options_xkb xkb_opt;
    struct options_desktop_integration desktop_opt;
    struct options_glx glx_opt;
    struct options_fonts_rendering fonts_opt;
    struct options_logging_extensions logging_opt;

    reset_all_options(&net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
        &xkb_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);
    int row, col;
    DWORD last_time = 0;

    for (row = 0; row < 9; ++row)
        option[row][0] = '\0';

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
                NK_WINDOW_NO_SCROLLBAR))
            {
                cr = nk_window_get_content_region(ctx);
                W = cr.w;
                H = cr.h;

                nk_layout_space_begin(ctx, NK_STATIC, H, 3);

                /* 3x3 tab header, pinned to the top */
                nk_layout_space_push(ctx, nk_rect(0, 0, W, 90));
                if (nk_group_begin(ctx, "tabs", NK_WINDOW_NO_SCROLLBAR)) {
                    nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(0,0));
                    nk_style_push_float(ctx, &ctx->style.button.rounding, 0);
                    for (row = 0; row < 3; ++row) {
                        nk_layout_row_dynamic(ctx, 30, 3);
                        for (col = 0; col < 3; ++col) {
                            int i = row * 3 + col;
                            if (current_tab == i) {
                                struct nk_style_item normal = ctx->style.button.normal;
                                ctx->style.button.normal = ctx->style.button.active;
                                if (nk_button_label(ctx, tab_names[i]))
                                    current_tab = i;
                                ctx->style.button.normal = normal;
                            } else if (nk_button_label(ctx, tab_names[i])) {
                                current_tab = i;
                            }
                        }
                    }
                    nk_style_pop_float(ctx);
                    nk_style_pop_vec2(ctx);
                    nk_group_end(ctx);
                }

                /* tab body: canvas fills the window, scrollable */
                nk_layout_space_push(ctx, nk_rect(0, 90, W, H - 90 - 30));
                if (nk_group_begin(ctx, "canvas", NK_WINDOW_BORDER)) {
                    if (current_tab == 0) {
                        tab_network_and_access_control(ctx, &net_opt);
                    } else if (current_tab == 1) {
                        tab_xdmcp(ctx, &xdmcp_opt);
                    } else if (current_tab == 2) {
                        tab_screen_and_windowing(ctx, &screen_opt);
                    } else if (current_tab == 3) {
                        tab_pointer_keyboard(ctx, &pointer_opt);
                    } else if (current_tab == 4) {
                        tab_xkb(ctx, &xkb_opt);
                    } else if (current_tab == 5) {
                        tab_desktop_integration(ctx, &desktop_opt);
                    } else if (current_tab == 6) {
                        tab_glx(ctx, &glx_opt);
                    } else if (current_tab == 7) {
                        tab_fonts_rendering(ctx, &fonts_opt);
                    } else if (current_tab == 8) {
                        tab_logging_extensions(ctx, &logging_opt);
                    } else {
                        nk_layout_row_dynamic(ctx, 25, 1);
                        nk_label(ctx, "Enter option", NK_TEXT_LEFT);
                        nk_layout_row_dynamic(ctx, 30, 1);
                        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
                            option[current_tab], 128, nk_filter_default);
                    }
                    nk_group_end(ctx);
                }

                /* OK / Cancel, gravity South */
                nk_layout_space_push(ctx, nk_rect(0, H - 30 - 15, W, 30 + 15));
                if (nk_group_begin(ctx, "buttons", NK_WINDOW_NO_SCROLLBAR)) {
                    struct nk_rect bb;
                    nk_layout_row_dynamic(ctx, 30, 3);
                    bb = nk_widget_bounds(ctx);
                    if (nk_button_label(ctx, "Reset"))
                        reset_all_options(&net_opt, &xdmcp_opt, &screen_opt, &pointer_opt,
                            &xkb_opt, &desktop_opt, &glx_opt, &fonts_opt, &logging_opt);
                    option_tooltip(ctx, bb, "Resets all configuration parameters in all sections to factory defaults");
                    if (nk_button_label(ctx, "OK"))
                        running = 0;
                    if (nk_button_label(ctx, "Cancel"))
                        running = 0;
                    nk_group_end(ctx);
                }

                nk_layout_space_end(ctx);
            }
        }
        nk_end(ctx);

        nk_gdi_render(nk_rgb(30, 30, 30));
    }

    nk_gdifont_del(font);
    ReleaseDC(wnd, dc);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
