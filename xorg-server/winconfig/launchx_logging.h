#ifndef LAUNCHX_LOGGING_H
#define LAUNCHX_LOGGING_H

#include "launchx_common.h"

#define NUM_EXTENSIONS 16
static const char *extension_names[NUM_EXTENSIONS] = {
    "SHAPE", "XTEST", "SECURITY", "XINERAMA", "XFIXES",
    "XFree86-Bigfont", "RENDER", "RANDR", "COMPOSITE", "DAMAGE",
    "MIT-SCREEN-SAVER", "DOUBLE-BUFFER", "RECORD", "DPMS",
    "X-Resource", "GLX"
};

static const char *extension_keys[NUM_EXTENSIONS] = {
    "shape", "xtest", "security", "xinerama", "xfixes",
    "xfree86bigfont", "render", "randr", "composite", "damage",
    "mitscreensaver", "doublebuffer", "record", "dpms",
    "xresource", "glx"
};

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

void tab_logging_extensions(struct nk_context *ctx,
    struct options_logging_extensions *opt);

#endif /* LAUNCHX_LOGGING_H */
