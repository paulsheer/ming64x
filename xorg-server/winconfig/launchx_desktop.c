#include "launchx_desktop.h"

void
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
