#include "terminal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../Nuklear/nukleardefault.h"
#include "../Nuklear/demo/gdi/nuklear_gdi.h"

#define TERMINAL_FONT_SIZE 14

static GdiFont *
terminal_font(void)
{
    static GdiFont *font = NULL;
    if (!font)
        font = nk_gdifont_create_fixed("Courier New", TERMINAL_FONT_SIZE);
    return font;
}

void
terminal_init(terminal *t)
{
    memset (t, '\0', sizeof (*t));
    t->draw_mode.fg_color = -1;
    t->draw_mode.bg_color = -1;
}

void
terminal_free(terminal *t)
{
    int r;
    for (r = 0; r < t->nrows; ++r)
        free(t->rows[r]);
    free(t->rows);
    t->rows = NULL;
    t->nrows = 0;
    t->ncols = 0;
    t->initialized = 0;
}

void
terminal_clear(terminal *t)
{
    int r, c;

    if (!t->initialized)
        return;

    for (r = 0; r < t->nrows; ++r) {
        for (c = 0; c < t->ncols; ++c)
            t->rows[r][c] = L'\0';
        t->rows[r][t->ncols] = L'\n';
    }

    t->cursor_row = 0;
    t->cursor_col = 0;
    t->saved_row = 0;
    t->saved_col = 0;
    t->scroll_top = 0;
    t->scroll_bottom = t->nrows - 1;

    t->draw_mode.bold = 0;
    t->draw_mode.fg_color = -1;
    t->draw_mode.bg_color = -1;
    t->draw_mode.underline = 0;

    t->select_point1_row = 0;
    t->select_point1_col = 0;
    t->select_point2_row = 0;
    t->select_point2_col = 0;
    t->select_active = 0;
    t->select_moved = 0;
    t->menu_open = 0;
    t->menu_show_copy = 0;

    t->esc_state = 0;
    t->csi_nparam = 0;
    t->csi_cur = -1;
    t->csi_private = 0;
    t->app_cursor_keys = 0;
    t->app_keypad_keys = 0;
}

static int
terminal_count_blank_rows(const terminal *t)
{
    int r, c;
    for (r = t->nrows - 1; r >= 0; --r) {
        for (c = 0; c < t->ncols; ++c)
            if (t->rows[r][c] != L' ' && t->rows[r][c] != L'\0')
                return t->nrows - 1 - r;
    }
    return t->nrows;
}

void
terminal_resize(terminal *t, int nrows, int ncols)
{
    int blank_rows = 0;
    int old_nrows = t->nrows;
    int old_ncols = t->ncols;
    int r, c;

    if (nrows == old_nrows && ncols == old_ncols)
        return;

    /* shrinking rows: scroll content up once per removed row so the bottom
       rows are kept and the top rows scroll off */
    blank_rows = terminal_count_blank_rows (t);
    if (nrows < old_nrows - blank_rows) {
        for (r = 0; r < old_nrows - blank_rows - nrows; ++r)
            terminal_scroll(t);
    }

    /* grow/shrink the columns of rows that survive */
    if (ncols != old_ncols) {
        int survive = nrows < old_nrows ? nrows : old_nrows;
        for (r = 0; r < survive; ++r) {
            t->rows[r] = (wchar_t*)realloc(t->rows[r],
                (size_t)(ncols + 1) * sizeof(wchar_t));
            for (c = old_ncols; c < ncols; ++c)
                t->rows[r][c] = L'\0';
            t->rows[r][ncols] = L'\n';
        }
    }

    /* drop rows that are being removed */
    for (r = nrows; r < old_nrows; ++r)
        free(t->rows[r]);

    t->rows = (wchar_t**)realloc(t->rows, (size_t)nrows * sizeof(wchar_t*));

    /* allocate and blank any newly added rows */
    for (r = old_nrows; r < nrows; ++r) {
        t->rows[r] = (wchar_t*)malloc((size_t)(ncols + 1) * sizeof(wchar_t));
        for (c = 0; c < ncols; ++c)
            t->rows[r][c] = L'\0';
        t->rows[r][ncols] = L'\n';
    }

    t->nrows = nrows;
    t->ncols = ncols;

    if (t->cursor_col >= ncols)
        t->cursor_col = ncols - 1;
    if (t->cursor_row >= nrows)
        t->cursor_row = nrows - 1;

    t->scroll_top = 0;
    t->scroll_bottom = nrows - 1;
}

void
terminal_scroll(terminal *t)
{
    int r, c;

    if (t->nrows < 1)
        return;

    for (r = 0; r + 1 < t->nrows; ++r)
        memcpy(t->rows[r], t->rows[r + 1], (size_t)(t->ncols + 1) * sizeof(wchar_t));

    for (c = 0; c < t->ncols; ++c)
        t->rows[t->nrows - 1][c] = L'\0';
    t->rows[t->nrows - 1][t->ncols] = L'\n';

    if (t->cursor_row > 0)
        t->cursor_row--;
}

/* xterm 16-color palette, packed 0xRRGGBB */
static const int ansi_palette[16] = {
    0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
    0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
    0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
    0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff
};

static int
ansi_256_color(int n)
{
    if (n < 16)
        return ansi_palette[n];
    if (n < 232) {
        static const int level[6] = {0, 95, 135, 175, 215, 255};
        int v = n - 16;
        return (level[v / 36] << 16) | (level[(v / 6) % 6] << 8) | level[v % 6];
    }
    {
        int v = 8 + 10 * (n - 232);
        return (v << 16) | (v << 8) | v;
    }
}

static void
sgr_apply(terminal *t)
{
    int i;

    if (t->csi_nparam == 0) {
        t->draw_mode.bold = 0;
        t->draw_mode.underline = 0;
        t->draw_mode.fg_color = -1;
        t->draw_mode.bg_color = -1;
        return;
    }

    for (i = 0; i < t->csi_nparam; ++i) {
        int code = t->csi_param[i];

        if (code == 0) {
            t->draw_mode.bold = 0;
            t->draw_mode.underline = 0;
            t->draw_mode.fg_color = -1;
            t->draw_mode.bg_color = -1;
        }
        else if (code == 1) {
            t->draw_mode.bold = 1;
        }
        else if (code == 4 || code == 21) {
            t->draw_mode.underline = 1;
        }
        else if (code == 22) {
            t->draw_mode.bold = 0;
        }
        else if (code == 24) {
            t->draw_mode.underline = 0;
        }
        else if (code == 39) {
            t->draw_mode.fg_color = -1;
        }
        else if (code == 49) {
            t->draw_mode.bg_color = -1;
        }
        else if (code >= 30 && code <= 37) {
            t->draw_mode.fg_color = ansi_palette[code - 30];
        }
        else if (code >= 40 && code <= 47) {
            t->draw_mode.bg_color = ansi_palette[code - 40];
        }
        else if (code >= 90 && code <= 97) {
            t->draw_mode.fg_color = ansi_palette[code - 90 + 8];
        }
        else if (code >= 100 && code <= 107) {
            t->draw_mode.bg_color = ansi_palette[code - 100 + 8];
        }
        else if (code == 38 || code == 48) {
            int isfg = (code == 38);
            if (i + 2 < t->csi_nparam && t->csi_param[i + 1] == 5) {
                int c = ansi_256_color(t->csi_param[i + 2]);
                if (isfg)
                    t->draw_mode.fg_color = c;
                else
                    t->draw_mode.bg_color = c;
                i += 2;
            }
            else if (i + 4 < t->csi_nparam && t->csi_param[i + 1] == 2) {
                int r = t->csi_param[i + 2] & 0xff;
                int g = t->csi_param[i + 3] & 0xff;
                int b = t->csi_param[i + 4] & 0xff;
                int c = (r << 16) | (g << 8) | b;
                if (isfg)
                    t->draw_mode.fg_color = c;
                else
                    t->draw_mode.bg_color = c;
                i += 4;
            }
        }
        /* other codes (italic, blink, reverse, ...) ignored */
    }
}

static int
csi_count(terminal *t)
{
    int v = (t->csi_nparam > 0) ? t->csi_param[0] : 1;
    return v <= 0 ? 1 : v;
}

static int
csi_mode(terminal *t)
{
    return (t->csi_nparam > 0) ? t->csi_param[0] : 0;
}

static void
scroll_region_up(terminal *t)
{
    int r, c;
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;

    if (top >= bottom)
        return;

    for (r = top; r < bottom; ++r)
        memcpy(t->rows[r], t->rows[r + 1], (size_t)(t->ncols + 1) * sizeof(wchar_t));
    for (c = 0; c < t->ncols; ++c)
        t->rows[bottom][c] = L'\0';
    t->rows[bottom][t->ncols] = L'\n';
}

static void
scroll_region_down(terminal *t)
{
    int r, c;
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;

    if (top >= bottom)
        return;

    for (r = bottom; r > top; --r)
        memcpy(t->rows[r], t->rows[r - 1], (size_t)(t->ncols + 1) * sizeof(wchar_t));
    for (c = 0; c < t->ncols; ++c)
        t->rows[top][c] = L'\0';
    t->rows[top][t->ncols] = L'\n';
}

static void
index_down(terminal *t)
{
    if (t->cursor_row >= t->scroll_bottom)
        scroll_region_up(t);
    else
        t->cursor_row++;
}

static void
index_up(terminal *t)
{
    if (t->cursor_row <= t->scroll_top)
        scroll_region_down(t);
    else
        t->cursor_row--;
}

static void
erase_line(terminal *t, int mode)
{
    int c, from = 0, to = t->ncols - 1;

    if (mode == 0)
        from = t->cursor_col;
    else if (mode == 1)
        to = t->cursor_col;

    for (c = from; c <= to; ++c)
        t->rows[t->cursor_row][c] = L'\0';
    t->rows[t->cursor_row][t->ncols] = L'\n';
}

static void
erase_display(terminal *t, int mode)
{
    int r, c;

    if (mode == 0) {
        for (c = t->cursor_col; c < t->ncols; ++c)
            t->rows[t->cursor_row][c] = L'\0';
        t->rows[t->cursor_row][t->ncols] = L'\n';
        for (r = t->cursor_row + 1; r < t->nrows; ++r) {
            for (c = 0; c < t->ncols; ++c)
                t->rows[r][c] = L'\0';
            t->rows[r][t->ncols] = L'\n';
        }
    }
    else if (mode == 1) {
        for (r = 0; r < t->cursor_row; ++r) {
            for (c = 0; c < t->ncols; ++c)
                t->rows[r][c] = L'\0';
            t->rows[r][t->ncols] = L'\n';
        }
        for (c = 0; c <= t->cursor_col; ++c)
            t->rows[t->cursor_row][c] = L'\0';
        t->rows[t->cursor_row][t->ncols] = L'\n';
    }
    else {
        for (r = 0; r < t->nrows; ++r) {
            for (c = 0; c < t->ncols; ++c)
                t->rows[r][c] = L'\0';
            t->rows[r][t->ncols] = L'\n';
        }
    }
}

static void
erase_chars(terminal *t, int n)
{
    int c, end = t->cursor_col + n;

    if (n < 1)
        return;
    if (end > t->ncols)
        end = t->ncols;
    for (c = t->cursor_col; c < end; ++c)
        t->rows[t->cursor_row][c] = L'\0';
}

static void
insert_chars(terminal *t, int n)
{
    int avail = t->ncols - t->cursor_col;
    int c;

    if (n > avail)
        n = avail;
    if (n < 1)
        return;

    memmove(&t->rows[t->cursor_row][t->cursor_col + n],
            &t->rows[t->cursor_row][t->cursor_col],
            (size_t)(avail - n) * sizeof(wchar_t));
    for (c = t->cursor_col; c < t->cursor_col + n; ++c)
        t->rows[t->cursor_row][c] = L'\0';
}

static void
delete_chars(terminal *t, int n)
{
    int avail = t->ncols - t->cursor_col;
    int c;

    if (n > avail)
        n = avail;
    if (n < 1)
        return;

    memmove(&t->rows[t->cursor_row][t->cursor_col],
            &t->rows[t->cursor_row][t->cursor_col + n],
            (size_t)(avail - n) * sizeof(wchar_t));
    for (c = t->ncols - n; c < t->ncols; ++c)
        t->rows[t->cursor_row][c] = L'\0';
}

static void
insert_lines(terminal *t, int n)
{
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;
    int room = bottom - t->cursor_row + 1;
    int r, c;

    if (t->cursor_row < top || t->cursor_row > bottom)
        return;
    if (n > room)
        n = room;
    if (n < 1)
        return;

    for (r = bottom; r >= t->cursor_row + n; --r)
        memcpy(t->rows[r], t->rows[r - n], (size_t)(t->ncols + 1) * sizeof(wchar_t));
    for (r = t->cursor_row; r < t->cursor_row + n; ++r) {
        for (c = 0; c < t->ncols; ++c)
            t->rows[r][c] = L'\0';
        t->rows[r][t->ncols] = L'\n';
    }
}

static void
delete_lines(terminal *t, int n)
{
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;
    int room = bottom - t->cursor_row + 1;
    int r, c;

    if (t->cursor_row < top || t->cursor_row > bottom)
        return;
    if (n > room)
        n = room;
    if (n < 1)
        return;

    for (r = t->cursor_row; r <= bottom - n; ++r)
        memcpy(t->rows[r], t->rows[r + n], (size_t)(t->ncols + 1) * sizeof(wchar_t));
    for (r = bottom - n + 1; r <= bottom; ++r) {
        for (c = 0; c < t->ncols; ++c)
            t->rows[r][c] = L'\0';
        t->rows[r][t->ncols] = L'\n';
    }
}

static void
csi_execute(terminal *t, wchar_t cmd)
{
    int n, row, col;

    switch (cmd) {
    case L'm':
        sgr_apply(t);
        break;

    /* ---- cursor movement ---- */
    case L'A': /* CUU up */
        n = csi_count(t);
        t->cursor_row -= n;
        if (t->cursor_row < 0)
            t->cursor_row = 0;
        break;
    case L'B': /* CUD down */
        n = csi_count(t);
        t->cursor_row += n;
        if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        break;
    case L'C': /* CUF right */
        n = csi_count(t);
        t->cursor_col += n;
        if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;
    case L'D': /* CUB left */
        n = csi_count(t);
        t->cursor_col -= n;
        if (t->cursor_col < 0)
            t->cursor_col = 0;
        break;
    case L'E': /* CNL next line */
        n = csi_count(t);
        t->cursor_col = 0;
        t->cursor_row += n;
        if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        break;
    case L'F': /* CPL previous line */
        n = csi_count(t);
        t->cursor_col = 0;
        t->cursor_row -= n;
        if (t->cursor_row < 0)
            t->cursor_row = 0;
        break;
    case L'G': /* CHA horizontal absolute */
        n = csi_count(t);
        t->cursor_col = n - 1;
        if (t->cursor_col < 0)
            t->cursor_col = 0;
        else if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;
    case L'H': /* CUP cursor position */
    case L'f': /* HVP */
        row = (t->csi_nparam > 0 && t->csi_param[0] > 0) ? t->csi_param[0] : 1;
        col = (t->csi_nparam > 1 && t->csi_param[1] > 0) ? t->csi_param[1] : 1;
        t->cursor_row = row - 1;
        t->cursor_col = col - 1;
        if (t->cursor_row < 0)
            t->cursor_row = 0;
        else if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        if (t->cursor_col < 0)
            t->cursor_col = 0;
        else if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;
    case L'd': /* VPA vertical position absolute */
        n = csi_count(t);
        t->cursor_row = n - 1;
        if (t->cursor_row < 0)
            t->cursor_row = 0;
        else if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        break;
    case L'a': /* HPR horizontal relative (right) */
        n = csi_count(t);
        t->cursor_col += n;
        if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;
    case L'e': /* VPR vertical relative (down) */
        n = csi_count(t);
        t->cursor_row += n;
        if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        break;
    case L'`': /* HPA horizontal position absolute */
        n = csi_count(t);
        t->cursor_col = n - 1;
        if (t->cursor_col < 0)
            t->cursor_col = 0;
        else if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;
    case L's': /* SCP save cursor */
        t->saved_row = t->cursor_row;
        t->saved_col = t->cursor_col;
        break;
    case L'u': /* RCP restore cursor */
        t->cursor_row = t->saved_row;
        t->cursor_col = t->saved_col;
        if (t->cursor_row < 0)
            t->cursor_row = 0;
        else if (t->cursor_row >= t->nrows)
            t->cursor_row = t->nrows - 1;
        if (t->cursor_col < 0)
            t->cursor_col = 0;
        else if (t->cursor_col >= t->ncols)
            t->cursor_col = t->ncols - 1;
        break;

    /* ---- erase ---- */
    case L'J': /* ED erase in display */
        erase_display(t, csi_mode(t));
        break;
    case L'K': /* EL erase in line */
        erase_line(t, csi_mode(t));
        break;
    case L'X': /* ECH erase chars */
        erase_chars(t, csi_count(t));
        break;

    /* ---- insert/delete ---- */
    case L'@': /* ICH insert chars */
        insert_chars(t, csi_count(t));
        break;
    case L'P': /* DCH delete chars */
        delete_chars(t, csi_count(t));
        break;
    case L'L': /* IL insert lines */
        insert_lines(t, csi_count(t));
        break;
    case L'M': /* DL delete lines */
        delete_lines(t, csi_count(t));
        break;

    /* ---- scroll ---- */
    case L'S': /* SU scroll up */
        n = csi_count(t);
        while (n-- > 0)
            scroll_region_up(t);
        break;
    case L'T': /* SD scroll down */
        n = csi_count(t);
        while (n-- > 0)
            scroll_region_down(t);
        break;
    case L'r': { /* DECSTBM set scroll region */
        int top = (t->csi_nparam > 0 && t->csi_param[0] > 0)
            ? t->csi_param[0] : 1;
        int bot = (t->csi_nparam > 1 && t->csi_param[1] > 0)
            ? t->csi_param[1] : t->nrows;
        if (top < 1)
            top = 1;
        if (bot > t->nrows)
            bot = t->nrows;
        if (top <= bot) {
            t->scroll_top = top - 1;
            t->scroll_bottom = bot - 1;
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;
    }

    default:
        break; /* ignore unknown CSI */
    }
}

void
terminal_print(terminal *t, const wchar_t *s)
{
    if (t->nrows < 1 || t->ncols < 1)
        return;

    while (*s) {
        wchar_t ch = *s++;

        switch (t->esc_state) {
        case 0: /* normal */
            if (ch == 0x1b) {
                t->esc_state = 1;
            }
            else if (ch == L'\n') {
                if (t->cursor_col == t->ncols)
                    t->rows[t->cursor_row][t->ncols] = L'\n';
                t->cursor_col = 0;
                index_down(t);
            }
            else if (ch == L'\r') {
                if (t->cursor_col == t->ncols)
                    t->rows[t->cursor_row][t->ncols] = L'\n';
                t->cursor_col = 0;
            }
            else if (ch == L'\t') {
                t->cursor_col = (t->cursor_col / 8 + 1) * 8;
                if (t->cursor_col >= t->ncols)
                    t->cursor_col = t->ncols - 1;
            }
            else if (ch == L'\b' || ch == 0x7f) {
                if (t->cursor_col > 0)
                    t->cursor_col--;
            }
            else if (ch == L'\v' || ch == L'\f') {
                index_down(t);   /* VT / FF: down one line, keep column */
            }
            else if (ch == 0x07 || ch == 0x0e || ch == 0x0f) {
                /* BEL / SO / SI: no glyph, no cursor movement */
            }
            else {
                if (t->cursor_col >= t->ncols) {
                    /* Real wrap that should have the line-break
                     * erased on copy. Therefore the only line-end
                     * that has a null character and not a new-line: */
                    if (t->cursor_col == t->ncols)
                        t->rows[t->cursor_row][t->ncols] = L'\0';
                    t->cursor_col = 0;
                    index_down(t);
                }
                t->rows[t->cursor_row][t->cursor_col] = ch;
                t->cursor_col++;
            }
            break;

        case 1: /* saw ESC */
            if (ch == L'[') {
                t->esc_state = 2;
                t->csi_nparam = 0;
                t->csi_cur = -1;
                t->csi_private = 0;
            }
            else if (ch == L']') {
                t->esc_state = 3;
            }
            else if (ch == L'(' || ch == L')' || ch == L'*' || ch == L'+') {
                t->esc_state = 4;   /* charset designation: swallow next byte */
            }
            else {
                if (ch == L'D') {           /* IND: down one line */
                    index_down(t);
                }
                else if (ch == L'M') {      /* RI: up, scroll down at top */
                    index_up(t);
                }
                else if (ch == L'E') {      /* NEL: down + col 0 */
                    t->cursor_col = 0;
                    index_down(t);
                }
                else if (ch == L'7') {      /* DECSC: save cursor */
                    t->saved_row = t->cursor_row;
                    t->saved_col = t->cursor_col;
                }
                else if (ch == L'8') {      /* DECRC: restore cursor */
                    t->cursor_row = t->saved_row;
                    t->cursor_col = t->saved_col;
                    if (t->cursor_row < 0)
                        t->cursor_row = 0;
                    else if (t->cursor_row >= t->nrows)
                        t->cursor_row = t->nrows - 1;
                    if (t->cursor_col < 0)
                        t->cursor_col = 0;
                    else if (t->cursor_col >= t->ncols)
                        t->cursor_col = t->ncols - 1;
                }
                else if (ch == L'=') {     /* DECKPAM: application keypad */
                    t->app_keypad_keys = 1;
                }
                else if (ch == L'>') {     /* DECKPNM: numeric keypad */
                    t->app_keypad_keys = 0;
                }
                t->esc_state = 0;
            }
            break;

        case 2: /* CSI */
            if (ch >= L'0' && ch <= L'9') {
                if (t->csi_cur < 0)
                    t->csi_cur = 0;
                t->csi_cur = t->csi_cur * 10 + (ch - L'0');
                if (t->csi_cur > 9999)
                    t->csi_cur = 9999;
            }
            else if (ch == L';' || ch == L':') {
                if (t->csi_nparam < 16)
                    t->csi_param[t->csi_nparam++] = t->csi_cur < 0 ? 0 : t->csi_cur;
                t->csi_cur = -1;
            }
            else if (ch == L'?') {
                t->csi_private = 1;
            }
            else if (ch >= 0x40 && ch <= 0x7e) {
                /* final byte */
                if (t->csi_nparam < 16)
                    t->csi_param[t->csi_nparam++] = t->csi_cur < 0 ? 0 : t->csi_cur;
                if (t->csi_private) {
                    /* DEC private modes (CSI ? Pm h/l) */
                    if ((ch == L'h' || ch == L'l') && t->csi_nparam > 0 &&
                        t->csi_param[0] == 1)
                        t->app_cursor_keys = (ch == L'h');   /* DECCKM */
                }
                else {
                    csi_execute(t, ch);
                }
                t->esc_state = 0;
            }
            else if (ch < 0x20) {
                /* stray control char: abort the sequence */
                t->esc_state = 0;
            }
            /* else: intermediate byte (0x20-0x2F) or param byte, ignore */
            break;

        case 3: /* OSC: swallow until BEL (0x07) or ST (ESC \) */
            if (ch == 0x07)
                t->esc_state = 0;
            else if (ch == 0x1b)
                t->esc_state = 1;
            break;

        case 4: /* charset designation: swallow the final byte */
            t->esc_state = 0;
            break;
        }
    }
}

static int
cell_selected(const terminal *t, int row, int col)
{
    int top_r, top_c, bot_r, bot_c;
    int cell_lin, top_lin, bot_lin;

    if (!t->select_active)
        return 0;

    if (t->select_point1_row < t->select_point2_row ||
        (t->select_point1_row == t->select_point2_row &&
         t->select_point1_col <= t->select_point2_col)) {
        top_r = t->select_point1_row;
        top_c = t->select_point1_col;
        bot_r = t->select_point2_row;
        bot_c = t->select_point2_col;
    }
    else {
        top_r = t->select_point2_row;
        top_c = t->select_point2_col;
        bot_r = t->select_point1_row;
        bot_c = t->select_point1_col;
    }

    cell_lin = row * t->ncols + col;
    top_lin = top_r * t->ncols + top_c;
    bot_lin = bot_r * t->ncols + bot_c;

    return cell_lin >= top_lin && cell_lin <= bot_lin;
}

static void
draw_row_segment(struct nk_command_buffer *canvas, const terminal *t,
    struct nk_user_font *font, float x, float y, float char_width,
    float line_height, int row, int c0, int c1,
    struct nk_color bg, struct nk_color fg, char *utf8, wchar_t *scratch)
{
    int len, n, c;
    struct nk_rect r;

    if (c1 < c0)
        return;

    /* blank cells are stored as L'\0'; render them as spaces so the
       UTF-8 string is not truncated by an embedded NUL */
    n = 0;
    for (c = c0; c <= c1; ++c)
        scratch[n++] = (unsigned int) t->rows[row][c] < 0x20U ? (t->rows[row][c] == L'\n' || t->rows[row][c] == L'\0' ? L' ' : L'?') : t->rows[row][c];

    len = WideCharToMultiByte(CP_UTF8, 0, scratch, n,
        utf8, t->ncols * 4, NULL, NULL);

    /* +0.5f keeps the segment rect a hair wider than the text so
       nk_draw_text's fit-to-rect clamp never truncates a glyph */
    r = nk_rect(x + c0 * char_width, y + row * line_height,
        (c1 - c0 + 1) * char_width + 0.5f, line_height);

    nk_draw_text(canvas, r, utf8, len, font, bg, fg);
}

static void
terminal_seed(terminal *t)
{
    (void) t;
}

void
terminal_draw(struct nk_context *ctx, terminal *t)
{
    struct nk_rect content, bounds;
    struct nk_command_buffer *canvas;
    struct nk_user_font *font = nk_gdifont_get_nk(terminal_font());
    float char_width, line_height, avail_h;
    int ncols, nrows, r;
    char *utf8;
    wchar_t *scratch;

    if (!font)
        return;

    line_height = font->height + 1.0f;
    char_width = font->width(font->userdata, font->height, "M", 1);
    if (char_width < 1.0f)
        char_width = 1.0f;

    /* fill the remaining vertical space of the canvas */
    content = nk_window_get_content_region(ctx);
    avail_h = (content.y + content.h) - nk_widget_bounds(ctx).y;
    if (avail_h < line_height)
        avail_h = line_height;

    nk_layout_row_dynamic(ctx, avail_h, 1);
    if (nk_widget(&bounds, ctx) == NK_WIDGET_INVALID)
        return;

    /* nk_widget trims row spacing off the widget height, so recompute the
       exact height down to the canvas bottom edge */
    avail_h = (content.y + content.h) - bounds.y;
    if (avail_h < line_height)
        avail_h = line_height;

    ncols = (int)(bounds.w / char_width);
    nrows = (int)(avail_h / line_height);
    if (ncols < 1) ncols = 1;
    if (nrows < 1) nrows = 1;

    if (ncols != t->ncols || nrows != t->nrows)
        terminal_resize(t, nrows, ncols);

    if (!t->initialized) {
        terminal_seed(t);
        t->initialized = 1;
    }

    utf8 = (char*)malloc((size_t)(t->ncols * 4));
    if (!utf8)
        return;

    scratch = (wchar_t*)malloc((size_t)(t->ncols + 1) * sizeof(wchar_t));
    if (!scratch) {
        free(utf8);
        return;
    }

    canvas = nk_window_get_canvas(ctx);
    nk_fill_rect(canvas, nk_rect(bounds.x, bounds.y, bounds.w, avail_h), 0,
        nk_rgb(0, 0, 0));

    t->draw_x = bounds.x;
    t->draw_y = bounds.y;
    t->char_width = char_width;
    t->line_height = line_height;

    for (r = 0; r < t->nrows; ++r) {
        int c, sel0 = -1, sel1 = -1;

        for (c = 0; c < t->ncols; ++c) {
            if (cell_selected(t, r, c)) {
                if (sel0 < 0)
                    sel0 = c;
                sel1 = c;
            }
        }

        if (sel0 < 0) {
            draw_row_segment(canvas, t, font, bounds.x, bounds.y, char_width,
                line_height, r, 0, t->ncols - 1,
                nk_rgb(0, 0, 0), nk_rgb(255, 255, 255), utf8, scratch);
        }
        else {
            struct nk_color un_bg = nk_rgb(0, 0, 0);
            struct nk_color un_fg = nk_rgb(255, 255, 255);
            struct nk_color sel_bg = nk_rgb(0, 0, 128);   /* dark blue */
            struct nk_color sel_fg = nk_rgb(255, 255, 255);

            nk_fill_rect(canvas,
                nk_rect(bounds.x + sel0 * char_width,
                    bounds.y + r * line_height,
                    (sel1 - sel0 + 1) * char_width, line_height),
                0, sel_bg);

            if (sel0 > 0)
                draw_row_segment(canvas, t, font, bounds.x, bounds.y,
                    char_width, line_height, r, 0, sel0 - 1, un_bg, un_fg,
                    utf8, scratch);
            draw_row_segment(canvas, t, font, bounds.x, bounds.y, char_width,
                line_height, r, sel0, sel1, sel_bg, sel_fg, utf8, scratch);
            if (sel1 < t->ncols - 1)
                draw_row_segment(canvas, t, font, bounds.x, bounds.y,
                    char_width, line_height, r, sel1 + 1, t->ncols - 1,
                    un_bg, un_fg, utf8, scratch);
        }
    }

    /* block cursor: inverse video at the cursor cell */
    if (t->cursor_row >= 0 && t->cursor_row < t->nrows &&
        t->cursor_col >= 0 && t->cursor_col < t->ncols) {
        struct nk_rect cell = nk_rect(bounds.x + t->cursor_col * char_width,
            bounds.y + t->cursor_row * line_height, char_width, line_height);
        nk_fill_rect(canvas, cell, 0, nk_rgb(255, 255, 255));

        if (t->rows[t->cursor_row][t->cursor_col] != L' ' && t->rows[t->cursor_row][t->cursor_col] != L'\0') {
            char mb[8];
            int len = WideCharToMultiByte(CP_UTF8, 0,
                &t->rows[t->cursor_row][t->cursor_col], 1,
                mb, sizeof mb, NULL, NULL);
            if (len > 0)
                nk_draw_text(canvas, cell, mb, len, font,
                    nk_rgb(255, 255, 255), nk_rgb(0, 0, 0));
        }
    }

    free(scratch);
    free(utf8);
}

void
terminal_mouse(struct nk_context *ctx, terminal *t)
{
    struct nk_vec2 pos;
    int row, col;

    if (t->menu_open)
        return;

    if (t->nrows < 1 || t->ncols < 1 || t->char_width < 1.0f)
        return;

    pos = ctx->input.mouse.pos;

    if (pos.x < t->draw_x || pos.y < t->draw_y ||
        pos.x >= t->draw_x + t->ncols * t->char_width ||
        pos.y >= t->draw_y + t->nrows * t->line_height)
        return;

    col = (int)((pos.x - t->draw_x) / t->char_width);
    row = (int)((pos.y - t->draw_y) / t->line_height);
    if (col < 0) col = 0;
    if (col >= t->ncols) col = t->ncols - 1;
    if (row < 0) row = 0;
    if (row >= t->nrows) row = t->nrows - 1;

    if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT)) {
        t->select_point1_row = row;
        t->select_point1_col = col;
        t->select_point2_row = row;
        t->select_point2_col = col;
        t->select_active = 1;
        t->select_moved = 0;
    } else
    if (nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
        if (row != t->select_point1_row || col != t->select_point1_col)
            t->select_moved = 1;
        t->select_point2_row = row;
        t->select_point2_col = col;
        t->select_active = 1;
    }
    if (nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT)) {
        if (!t->select_moved)
            t->select_active = 0;
        else {
            t->select_point2_row = row;
            t->select_point2_col = col;
        }
    }
}

static void
terminal_copy(terminal *t)
{
    int top_r, top_c, bot_r, bot_c;
    int r, c, c0, c1, pos, total;
    wchar_t *buf;
    HGLOBAL h;
    wchar_t *dst;

    if (!t->select_active || t->nrows < 1 || t->ncols < 1)
        return;

    if (t->select_point1_row < t->select_point2_row ||
        (t->select_point1_row == t->select_point2_row &&
         t->select_point1_col <= t->select_point2_col)) {
        top_r = t->select_point1_row; top_c = t->select_point1_col;
        bot_r = t->select_point2_row; bot_c = t->select_point2_col;
    } else {
        top_r = t->select_point2_row; top_c = t->select_point2_col;
        bot_r = t->select_point1_row; bot_c = t->select_point1_col;
    }

    total = (bot_r - top_r + 1) * (t->ncols + 2);
    buf = (wchar_t*)malloc((size_t)(total + 1) * sizeof(wchar_t));
    if (!buf)
        return;

    pos = 0;
    for (r = top_r; r <= bot_r; ++r) {
        c0 = (r == top_r) ? top_c : 0;
        c1 = (r == bot_r) ? bot_c : t->ncols;
        for (c = c0; c <= c1; ++c) {
            if (t->rows[r][c] == L'\0') {
                if (c < t->ncols) {
                    buf[pos++] = L'\r';
                    buf[pos++] = L'\n';
                }
                break;
            }
            if (c == t->ncols || t->rows[r][c] == L'\n') {
                buf[pos++] = L'\r';
                buf[pos++] = L'\n';
                break;
            }
            buf[pos++] = t->rows[r][c];
        }
    }
    buf[pos] = L'\0';

    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        h = GlobalAlloc(GMEM_MOVEABLE, (size_t)(pos + 1) * sizeof(wchar_t));
        if (h) {
            dst = (wchar_t*)GlobalLock(h);
            if (dst) {
                memcpy(dst, buf, (size_t)(pos + 1) * sizeof(wchar_t));
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
                h = NULL;
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
    }
    free(buf);
}

int
terminal_menu(struct nk_context *ctx, terminal *t)
{
    struct nk_rect grid;
    struct nk_rect item;
    int paste = 0;

    if (t->ncols < 1 || t->nrows < 1 || t->char_width < 1.0f) {
        t->menu_open = 0;
        t->menu_show_copy = 0;
        return 0;
    }

    grid = nk_rect(t->draw_x, t->draw_y,
        t->ncols * t->char_width, t->nrows * t->line_height);

    /* On the frame the menu opens, decide whether the click landed on an
       existing selection; freeze the choice so moving toward the item
       cannot make Copy disappear. Mirrors nk_contextual_begin's trigger. */
    if (!t->menu_open &&
        (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_RIGHT, grid) ||
         (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT) &&
          nk_input_is_mouse_hovering_rect(&ctx->input, grid)))) {
        struct nk_vec2 pos = ctx->input.mouse.pos;
        int row, col;
        col = (int)((pos.x - t->draw_x) / t->char_width);
        row = (int)((pos.y - t->draw_y) / t->line_height);
        if (col < 0) col = 0;
        if (col >= t->ncols) col = t->ncols - 1;
        if (row < 0) row = 0;
        if (row >= t->nrows) row = t->nrows - 1;
        t->menu_show_copy = cell_selected(t, row, col);
    }

    if (nk_contextual_begin(ctx, 0, nk_vec2(120, t->menu_show_copy ? (52 + 10) : (28 + 10)), grid)) {
        t->menu_open = 1;
        if (t->menu_show_copy) {
            nk_layout_row_dynamic(ctx, 24, 1);
            item = nk_widget_bounds(ctx);   /* peek bounds of the next widget */
            nk_button_label_styled(ctx, &ctx->style.contextual_button, "Copy");
            if ((nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT) ||
                 nk_input_is_mouse_released(&ctx->input, NK_BUTTON_RIGHT)) &&
                nk_input_is_mouse_hovering_rect(&ctx->input, item)) {
                terminal_copy(t);
                nk_contextual_close(ctx);
            }
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        item = nk_widget_bounds(ctx);
        nk_button_label_styled(ctx, &ctx->style.contextual_button, "Paste");
        if ((nk_input_is_mouse_released(&ctx->input, NK_BUTTON_LEFT) ||
             nk_input_is_mouse_released(&ctx->input, NK_BUTTON_RIGHT)) &&
            nk_input_is_mouse_hovering_rect(&ctx->input, item)) {
            paste = 1;
            nk_contextual_close(ctx);
        }
        nk_contextual_end(ctx);
    } else {
        t->menu_open = 0;
        t->menu_show_copy = 0;
    }
    return paste;
}
