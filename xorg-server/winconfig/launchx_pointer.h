#ifndef LAUNCHX_POINTER_H
#define LAUNCHX_POINTER_H

#include "launchx_common.h"

struct options_pointer_keyboard {
    int emulate3buttons_enabled;
    char emulate3buttons_timeout[16];
    int winkill_enabled;
    int unixkill_enabled;
    int grab_alt_tab;
    int grab_left_win_key;
    int grab_right_win_key;
    int ignoreinput_enabled;
    int autorepeat_enabled;
    char pointer_acceleration[16];
    char pointer_threshold[16];
    int bell;
    char autorepeat_delay[16];
    char autorepeat_interval[16];
};

void tab_pointer_keyboard(struct nk_context *ctx, struct options_pointer_keyboard *opt);

#endif /* LAUNCHX_POINTER_H */
