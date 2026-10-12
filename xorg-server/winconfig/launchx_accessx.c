#include "launchx_accessx.h"

static const char *ax_ctrl_names[NUM_AX_CTRL] = {
    "Repeat keys", "Slow keys", "Bounce keys", "Sticky keys", "Mouse keys",
    "Mouse keys acceleration", "AccessX keys", "AccessX timeout", "AccessX feedback"
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

static const char *ax_opt_names[NUM_AX_OPT] = {
    "StickyKeys press feedback", "StickyKeys accept feedback",
    "Feature on/off feedback", "SlowKeys warning feedback",
    "Indicator feedback", "StickyKeys feedback",
    "Two keys (disable StickyKeys)", "Latch to lock",
    "StickyKeys release feedback", "StickyKeys reject feedback",
    "BounceKeys reject feedback", "Dumb bell feedback"
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

void
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
