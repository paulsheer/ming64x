#include "launchx_logging.h"

void
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
