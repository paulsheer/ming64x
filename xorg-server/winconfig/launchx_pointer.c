#include "launchx_pointer.h"

void
tab_pointer_keyboard(struct nk_context *ctx, struct options_pointer_keyboard *opt)
{
    struct nk_rect b;

    heading(ctx, "Pointer & keyboard input");

    checkbox_option(ctx, "Emulate 3-button mouse (-emulate3buttons)", &opt->emulate3buttons_enabled, "Emulate a middle button when both buttons are pressed together.");

    text_option(ctx, "Emulate timeout (ms)", opt->emulate3buttons_timeout, (int)sizeof(opt->emulate3buttons_timeout), "Timeout (ms) within which pressing both buttons counts as a\nmiddle click.");

    checkbox_option(ctx, "Alt+F4 exits server (-winkill)", &opt->winkill_enabled, "Allow Alt+F4 to exit the X server.");

    checkbox_option(ctx, "Grab Alt+Tab (-grab-alt-tab)", &opt->grab_alt_tab, "Grab Alt+Tab and pass it to the X server.");

    checkbox_option(ctx, "Grab left Windows key (-grab-left-win-key)", &opt->grab_left_win_key, "Grab the left Windows key and pass it to the X server.");

    checkbox_option(ctx, "Grab right Windows key (-grab-right-win-key)", &opt->grab_right_win_key, "Grab the right Windows key and pass it to the X server.");

    checkbox_option(ctx, "Ctrl+Alt+Backspace exits server (-unixkill)", &opt->unixkill_enabled, "Allow Ctrl+Alt+Backspace to exit the X server.");

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
