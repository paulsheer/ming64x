/*
 * xtest - minimal self-contained X11 test client.
 *
 * Opens a borderless, window-manager-bypassed window centered on the root
 * window and covering half of its area, then draws "TEST SUCCESS" in red at
 * 100-pixel glyph height using an embedded 10x20 bitmap font.  No fontconfig,
 * no local font files, and no server-side fonts are required.  Exits on any
 * key or button press.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

#define FONT_W 10
#define FONT_H 20
#define CELL   5               /* each font pixel -> CELL x CELL px (glyphs 100px tall) */
#define GAP    5               /* inter-character spacing in px */
#define AREA_SCALE 0.7071067811865476  /* 1/sqrt(2): half the root area */

/* 10x20 glyph bitmaps; bit (FONT_W-1) = leftmost column, row 0 = top. */
static const unsigned short glyph_T[FONT_H] = {
    0x3FF, 0x3FF, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030,
    0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030, 0x030
};
static const unsigned short glyph_E[FONT_H] = {
    0x3FF, 0x3FF,
    0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300,
    0x3FC, 0x3FC,
    0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300,
    0x3FF, 0x3FF
};
static const unsigned short glyph_S[FONT_H] = {
    0x0FE, 0x1FF, 0x300, 0x300, 0x300,
    0x180, 0x0C0, 0x060, 0x030, 0x018, 0x00C, 0x006,
    0x003, 0x003, 0x003, 0x003, 0x003, 0x007,
    0x1FE, 0x3FC
};
static const unsigned short glyph_U[FONT_H] = {
    0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303,
    0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303, 0x303,
    0x1FE, 0x1FE
};
static const unsigned short glyph_C[FONT_H] = {
    0x3FF, 0x3FF,
    0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300,
    0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300, 0x300,
    0x3FF, 0x3FF
};

static const unsigned short *
glyph_for(int c)
{
    switch (c) {
    case 'T': return glyph_T;
    case 'E': return glyph_E;
    case 'S': return glyph_S;
    case 'U': return glyph_U;
    case 'C': return glyph_C;
    default:  return NULL;       /* space / unknown -> blank */
    }
}

int
main(void)
{
    Display *dpy;
    int screen;
    Window root, win;
    XSetWindowAttributes attr;
    GC gc;
    XColor red;
    const char *text = "TEST SUCCESS";
    int sw, sh, w, h, x, y;
    int glyph_px, text_h, text_w, tx, ty;
    int n, i, r, c;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "xtest: cannot open display %s\n",
                XDisplayName(NULL));
        return 1;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);

    w = (int)(sw * AREA_SCALE + 0.5);
    h = (int)(sh * AREA_SCALE + 0.5);
    x = (sw - w) / 2;
    y = (sh - h) / 2;

    attr.override_redirect = True;
    attr.background_pixel = WhitePixel(dpy, screen);
    attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask
                    | StructureNotifyMask;

    win = XCreateWindow(dpy, root, x, y, (unsigned)w, (unsigned)h, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &attr);
    if (!win) {
        fprintf(stderr, "xtest: XCreateWindow failed\n");
        XCloseDisplay(dpy);
        return 1;
    }

    red.red = 65535;
    red.green = 0;
    red.blue = 0;
    red.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, screen), &red);

    gc = XCreateGC(dpy, win, 0, NULL);
    XSetForeground(dpy, gc, red.pixel);

    n = (int)strlen(text);
    glyph_px = FONT_W * CELL;
    text_h = FONT_H * CELL;
    text_w = n * glyph_px + (n - 1) * GAP;
    tx = (w - text_w) / 2;
    ty = (h - text_h) / 2;

    XMapWindow(dpy, win);

    for (;;) {
        XEvent ev;

        for (i = 0; i < n; i++) {
            const unsigned short *g = glyph_for(text[i]);
            if (!g)
                continue;
            for (r = 0; r < FONT_H; r++)
                for (c = 0; c < FONT_W; c++)
                    if (g[r] & (1u << (FONT_W - 1 - c)))
                        XFillRectangle(dpy, win, gc,
                                       tx + i * (glyph_px + GAP) + c * CELL,
                                       ty + r * CELL, CELL, CELL);
        }
        XFlush(dpy);

        XNextEvent(dpy, &ev);
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
