/*
 * dib.c — DIB / DIBV5 loader.
 *
 * Parses BITMAPINFOHEADER (40 bytes) or BITMAPV5HEADER (124 bytes) followed
 * by 24-bit BGR or 32-bit BGRA pixel data.  Returns top-down RGBA.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "dib.h"

unsigned char *dibimg_load(const unsigned char *data, size_t len,
                           int *w, int *h)
{
    if (!data || len < 40) return NULL;

    /* Parse BITMAPINFOHEADER fields */
    uint32_t hdr_size  = *(uint32_t *)(data + 0);
    int32_t  bw        = *(int32_t  *)(data + 4);
    int32_t  bh        = *(int32_t  *)(data + 8);
    uint16_t bitcount  = *(uint16_t *)(data + 14);
    uint32_t comp      = *(uint32_t *)(data + 16);

    if (hdr_size < 40 || hdr_size > len) return NULL;
    if (bw <= 0 || bw > 16384) return NULL;

    int top_down = (bh < 0);
    int height   = top_down ? -bh : bh;
    if (height <= 0 || height > 16384) return NULL;
    if (bitcount != 24 && bitcount != 32) return NULL;
    if (comp != 0 /* BI_RGB */ && comp != 3 /* BI_BITFIELDS */) return NULL;

    int row_bytes = ((int)bw * bitcount + 31) / 32 * 4;
    size_t img_size = (size_t)row_bytes * (size_t)height;
    if (hdr_size + img_size > len) return NULL;

    size_t npixels = (size_t)bw * (size_t)height;
    unsigned char *pixels = (unsigned char *)malloc(npixels * 4);
    if (!pixels) return NULL;

    const unsigned char *src = data + hdr_size;
    int bpp = bitcount / 8;

    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        for (int x = 0; x < bw; x++) {
            int si = src_y * row_bytes + x * bpp;
            int di = (y * bw + x) * 4;
            pixels[di + 0] = src[si + 2];  /* B → R */
            pixels[di + 1] = src[si + 1];  /* G */
            pixels[di + 2] = src[si + 0];  /* R → B */
            pixels[di + 3] = (bpp == 4) ? src[si + 3] : 255;
        }
    }

    *w = bw;
    *h = height;
    return pixels;
}
