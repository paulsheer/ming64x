#ifndef TERMINAL_H
#define TERMINAL_H

#include <stddef.h>

struct nk_context;

struct draw_mode {
    int bold;
    int fg_color;
    int bg_color;
    int underline;
};

typedef struct terminal {
    wchar_t **rows;
    int nrows;
    int ncols;
    int initialized;
    int cursor_row;
    int cursor_col;
    int saved_row;
    int saved_col;
    int scroll_top;
    int scroll_bottom;
    struct draw_mode draw_mode;

    int select_point1_row;
    int select_point1_col;
    int select_point2_row;
    int select_point2_col;
    int select_active;          /* a selection is active (endpoints valid) */
    int select_moved;           /* mouse left the start cell during this drag */
    int menu_open;              /* right-click context menu is showing */
    int menu_show_copy;         /* context menu is showing the Copy item */

    /* ANSI escape parser state (persists across terminal_print calls) */
    int esc_state;          /* 0 normal, 1 saw ESC, 2 CSI, 3 OSC */
    int csi_param[16];
    int csi_nparam;
    int csi_cur;            /* digit accumulator, -1 = no digit seen yet */
    int csi_private;        /* saw '?' (private-mode CSI) */

    int app_cursor_keys;    /* DECCKM: application cursor keys */
    int app_keypad_keys;    /* DECKPAM/DECKPNM: application keypad */

    /* cached draw geometry, for mapping mouse coords to grid cells */
    float draw_x;
    float draw_y;
    float char_width;
    float line_height;
} terminal;

void terminal_init(terminal *t);
void terminal_free(terminal *t);
void terminal_clear(terminal *t);
void terminal_resize(terminal *t, int nrows, int ncols);
void terminal_scroll(terminal *t);
void terminal_print(terminal *t, const wchar_t *s);
void terminal_draw(struct nk_context *ctx, terminal *t);
void terminal_mouse(struct nk_context *ctx, terminal *t);
int terminal_menu(struct nk_context *ctx, terminal *t);

#endif /* TERMINAL_H */
