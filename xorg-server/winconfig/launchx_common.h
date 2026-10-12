#ifndef LAUNCHX_COMMON_H
#define LAUNCHX_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Nuklear/nukleardefault.h"
#include "terminal.h"
#include "ssh.h"

int inaddr_validate(const char *s, int *family);

/* Shared globals (defined in launchx.c). */
extern int g_focus_idx;
extern int g_focus_seq;
extern int g_confirm_reset;
extern char g_hyperv_error[1024];

/* Shared widget helpers (defined in launchx.c). */
void option_tooltip(struct nk_context *ctx, const struct nk_rect bounds, const char *text);
int focus_pick(struct nk_context *ctx, struct nk_rect *b);
void focus_ring(struct nk_context *ctx, const struct nk_rect b);
int button_option(struct nk_context *ctx, const char *label);
void checkbox_option(struct nk_context *ctx, const char *label, int *value, const char *tooltip);
void field_context_menu(struct nk_context *ctx, struct nk_rect bounds,
    char *buffer, int buffer_size, int copy_ok, int paste_ok, const void *owner);
nk_flags text_option_cell(struct nk_context *ctx, const char *label, char *buffer,
    const int buffer_size, const char *tooltip, nk_flags align);
nk_flags text_option(struct nk_context *ctx, const char *label, char *buffer,
    const int buffer_size, const char *tooltip);
nk_bool readonly_filter(const struct nk_text_edit *box, nk_rune unicode);
void readonly_option(struct nk_context *ctx, const char *label, const char *value, const char *tooltip);
void combobox_option(struct nk_context *ctx, const char *label, const char *const *items,
    const int count, int *selected, const char *tooltip);
void heading(struct nk_context *ctx, const char *text);

/* Bold font accessor for tabs that need the bold UI font.  GdiFont is opaque
   outside launchx.c, which owns the NK_GDI_IMPLEMENTATION translation unit. */
struct nk_user_font *launchx_bold_font(void);

/* Configuration directory helpers (defined in launchx.c). */
int config_dir(char *out, const size_t outsz);
int config_dir_real(char *out, size_t outsz);

#endif /* LAUNCHX_COMMON_H */
