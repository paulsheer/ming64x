/*
 * dib.c — DIB / DIBV5 loader.
 *
 * Parses BITMAPINFOHEADER (40 bytes) or BITMAPV5HEADER (124 bytes) followed
 * by 24-bit or 32-bit pixel data.  Returns top-down RGBA.
 *
 * Colour channels are extracted using the DIB's own colour masks (for
 * BI_BITFIELDS) or the conventional BGR(A) byte order (for BI_RGB), so the
 * result matches what GDI renders on screen.  The three BI_BITFIELDS masks
 * are documented in red, green, blue order.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "dib.h"

/* Number of trailing zero bits in a channel mask == the shift that moves the
   selected channel into the low bits. */
static int
mask_shift(uint32_t mask)
{
    int s = 0;
    while (mask && !(mask & 1u)) {
        mask >>= 1;
        s++;
    }
    return s;
}

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

    /* BI_BITFIELDS with a plain BITMAPINFOHEADER stores the three colour
       masks between the header and the pixel data; V4/V5 embed them. */
    size_t pixel_off = hdr_size;
    if (comp == 3 /* BI_BITFIELDS */ && hdr_size == 40)
        pixel_off += 3 * sizeof(uint32_t);

    size_t img_size = (size_t)row_bytes * (size_t)height;
    if (pixel_off + img_size > len) return NULL;

    size_t npixels = (size_t)bw * (size_t)height;
    unsigned char *pixels = (unsigned char *)malloc(npixels * 4);
    if (!pixels) return NULL;

    const unsigned char *src = data + pixel_off;
    int bpp = bitcount / 8;

    /* Colour masks: BI_BITFIELDS documents them in red, green, blue order
       (offsets 40/44/48); BI_RGB uses the conventional BGR(A) layout. */
    uint32_t rmask, gmask, bmask;
    if (comp == 3 /* BI_BITFIELDS */) {
        rmask = *(uint32_t *)(data + 40);
        gmask = *(uint32_t *)(data + 44);
        bmask = *(uint32_t *)(data + 48);
    } else {
        rmask = 0x00FF0000;
        gmask = 0x0000FF00;
        bmask = 0x000000FF;
    }

    int rshift = mask_shift(rmask);
    int gshift = mask_shift(gmask);
    int bshift = mask_shift(bmask);

    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        for (int x = 0; x < bw; x++) {
            int si = src_y * row_bytes + x * bpp;
            uint32_t pix = src[si + 0]
                         | ((uint32_t)src[si + 1] << 8)
                         | ((uint32_t)src[si + 2] << 16)
                         | (bpp == 4 ? ((uint32_t)src[si + 3] << 24) : 0);
            int di = (y * bw + x) * 4;
            pixels[di + 0] = (unsigned char)((pix & rmask) >> rshift);
            pixels[di + 1] = (unsigned char)((pix & gmask) >> gshift);
            pixels[di + 2] = (unsigned char)((pix & bmask) >> bshift);
            pixels[di + 3] = (bpp == 4) ? src[si + 3] : 255;
        }
    }

    *w = bw;
    *h = height;
    return pixels;
}
