#ifndef LAUNCHX_ACCESSX_H
#define LAUNCHX_ACCESSX_H

#include "launchx_common.h"

#define NUM_AX_CTRL 9
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

#define NUM_AX_OPT 12
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

struct options_accessx {
    int enabled;
    char timeout[16];
    int timeout_mask[NUM_AX_CTRL];
    int feedback;
    int options_mask[NUM_AX_OPT];
};

void tab_accessx(struct nk_context *ctx, struct options_accessx *opt);

#endif /* LAUNCHX_ACCESSX_H */
