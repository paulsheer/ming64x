#include "launchx_sshlogin.h"

static void
password_option_cell(struct nk_context *ctx, const char *label, char *password,
    char *mask, const int size, const char *tooltip)
{
    struct nk_rect b;
    int old_len, new_len, i, s;
    int focused;
    nk_flags ret;
    char rebuilt[128];

    /* A password loaded from launchx.cnf arrives with the mask still empty;
       fill it with asterisks so the field shows instead of appearing blank.
       After any edit the mask stays in lockstep with the password, so an
       empty mask with a non-empty password only occurs right after load. */
    if (mask[0] == '\0' && password[0] != '\0') {
        int plen = (int)strlen(password);
        if (plen > size - 1)
            plen = size - 1;
        for (i = 0; i < plen; ++i)
            mask[i] = '*';
        mask[plen] = '\0';
    }
    old_len = (int)strlen(mask);

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
    field_context_menu(ctx, b, mask, size, 0, 1, mask);

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

void
tab_ssh_login(struct nk_context *ctx, struct options_ssh_login *opt,
    terminal *term, ssh_session *ssh,
    struct options_network_and_access_control *net,
    struct options_audio *audio)
{
    int cols_before = term->ncols;
    int rows_before = term->nrows;
    struct nk_rect b;

    heading(ctx, "SSH login");

    nk_layout_row_dynamic(ctx, 30, 4);
    text_option_cell(ctx, "SSH Connect IP", opt->host, sizeof(opt->host),
        "Hostname or IP address of the SSH server to connect to", NK_TEXT_LEFT);
    text_option_cell(ctx, "SSH Connect port", opt->port, sizeof(opt->port),
        "TCP port of the SSH server (default 22)", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 30, 4);
    text_option_cell(ctx, "Login username", opt->username, sizeof(opt->username),
        "Username to authenticate with on the SSH server", NK_TEXT_LEFT);
    password_option_cell(ctx, "Login password", opt->password, opt->password_mask,
        sizeof(opt->password),
        "Password to authenticate with on the SSH server");

    nk_layout_row_dynamic(ctx, 30, 2);
    if (ssh_session_is_active(ssh))
        nk_widget_disable_begin(ctx);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, "X11 forwarding", &opt->x11_forwarding);
    option_tooltip(ctx, b, "Enable X11 forwarding over the SSH connection so X11 clients on the\nremote host can connect back to this X server");
    if (ssh_session_is_active(ssh))
        nk_widget_disable_end(ctx);
    b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, "Save password", &opt->save_password);
    option_tooltip(ctx, b, "Save the login password (encrypted) in launchx.cnf.\nWhen off, the password is not written to the config file.");

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
        unsigned char cookie[256];
        int have_cookie;

        if (inaddr_validate(opt->host, NULL)) {
            snprintf(ssh->display_error, sizeof ssh->display_error,
                     "Invalid IP address \"%s\"", opt->host);
            InterlockedExchange(&ssh->display_error_pending, 1);
        } else {
            int port = atoi(opt->port);
            if (port < 1 || port > 65535)
                port = 22;

            have_cookie = audio->audio_enabled && read_pulse_cookie(cookie);

            ssh_session_start(ssh, opt->host, opt->username, opt->password,
                port, net->listeningport_sel, opt->x11_forwarding,
                have_cookie, atoi(audio->pulseport),
                have_cookie ? cookie : NULL);
            ssh_request_resize(ssh, term->ncols, term->nrows);
        }
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
