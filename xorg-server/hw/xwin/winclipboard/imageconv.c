/*
 *Copyright (C) 2024 The VcXsrv Project
 *
 *Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 *"Software"), to deal in the Software without restriction, including
 *without limitation the rights to use, copy, modify, merge, publish,
 *distribute, sublicense, and/or sell copies of the Software, and to
 *permit persons to whom the Software is furnished to do so, subject to
 *the following conditions:
 *
 *The above copyright notice and this permission notice shall be
 *included in all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Image clipboard conversion: the Win32 clipboard exposes images as a single
 * native format, CF_DIB (a packed device-independent bitmap: a
 * BITMAPINFOHEADER, optional colour masks/palette, then pixel data).  The X11
 * world negotiates image data through MIME target atoms (image/bmp, image/png,
 * image/jpeg).  This unit converts CF_DIB into each of those targets when an
 * X11 client requests the Win32 clipboard selection:
 *
 *   image/bmp  - a .bmp file is exactly a 14-byte BITMAPFILEHEADER prepended to
 *                the DIB, so this is a pure header operation with no codec.
 *   image/png  - re-encoded from the DIB via GDI+ (lossless, alpha preserved).
 *   image/jpeg - re-encoded from the DIB via GDI+ (lossy, no alpha channel).
 *
 * GDI+ is reached through its flat C API so that this stays a C translation
 * unit and only adds a single import library (-lgdiplus) to the link.
 */

#define COBJMACROS

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif

#include "internal.h"
#include <limits.h>
#include <objbase.h>
#include "misc.h"
#include "winmsg.h"

/* stb_image — public-domain single-header image decoder (PNG, JPEG, BMP,
   and more).  Define the implementation in this translation unit. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* stb_image_write — public-domain single-header image writer (PNG, JPEG, etc.).
   Define the implementation in this translation unit. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/*
 * Size in bytes of everything in a packed DIB before the pixel data: the info
 * header, any BI_BITFIELDS colour masks, and the colour table.  This is also
 * the bfOffBits value a .bmp file needs.
 */
static SIZE_T
winClipboardDibPixelOffset(const BITMAPINFOHEADER *pbih)
{
    SIZE_T offset = pbih->biSize;

    /* A 40-byte BITMAPINFOHEADER with BI_BITFIELDS is followed by 3 colour
       masks. BITMAPV4/V5 headers carry their masks inside the header itself. */
    if (pbih->biSize == sizeof(BITMAPINFOHEADER) &&
        pbih->biCompression == BI_BITFIELDS) {
        if (offset > (SIZE_T) -1 - (3 * sizeof(DWORD)))
            return (SIZE_T) -1;
        offset += 3 * sizeof(DWORD);
    }

    /* Colour table: present for indexed depths, optional otherwise. */
    if (pbih->biBitCount <= 8) {
        DWORD entries = pbih->biClrUsed ? pbih->biClrUsed
                                        : (1u << pbih->biBitCount);
        if (entries > (((SIZE_T) -1 - offset) / sizeof(RGBQUAD)))
            return (SIZE_T) -1;
        offset += entries * sizeof(RGBQUAD);
    }
    else if (pbih->biClrUsed) {
        if (pbih->biClrUsed > (((SIZE_T) -1 - offset) / sizeof(RGBQUAD)))
            return (SIZE_T) -1;
        offset += pbih->biClrUsed * sizeof(RGBQUAD);
    }

    return offset;
}

/*
 * Wrap a packed DIB as a .bmp file (image/bmp).  No codec needed: prepend a
 * BITMAPFILEHEADER (14 bytes, packed in the SDK so sizeof == 14) and copy the
 * DIB verbatim, which preserves the exact colour depth and any alpha bytes.
 */
static BOOL
winClipboardDibToBmp(const BITMAPINFOHEADER *pbih, SIZE_T cbDib,
                     void **ppvData, unsigned long *pcbData)
{
    SIZE_T offBits;
    SIZE_T cbFile;
    unsigned char *out;
    BITMAPFILEHEADER *pbfh;

    offBits = winClipboardDibPixelOffset(pbih);
    if (offBits > cbDib)
        return FALSE;                   /* malformed: header runs past the data */

    /* Guard against pathological sizes before allocating or narrowing. */
    if (cbDib > (SIZE_T) ULONG_MAX - sizeof(BITMAPFILEHEADER))
        return FALSE;

    cbFile = sizeof(BITMAPFILEHEADER) + cbDib;
    out = malloc(cbFile);
    if (!out)
        return FALSE;

    pbfh = (BITMAPFILEHEADER *) out;
    pbfh->bfType = 0x4D42;              /* 'BM' */
    pbfh->bfSize = (DWORD) cbFile;
    pbfh->bfReserved1 = 0;
    pbfh->bfReserved2 = 0;
    pbfh->bfOffBits = (DWORD) (sizeof(BITMAPFILEHEADER) + offBits);

    memcpy(out + sizeof(BITMAPFILEHEADER), pbih, cbDib);

    *ppvData = out;
    *pcbData = (unsigned long) cbFile;
    return TRUE;
}

/*
 * Re-encode a packed DIB to PNG (image/png).
 * Uses the BMP round-trip: DIB → .bmp → stbi_load (RGBA) → stbi_write_png.
 * Not the most memory-efficient approach, but it handles every DIB format
 * that stb_image's BMP reader supports without writing a dedicated DIB→RGBA
 * converter.
 */
static BOOL
winClipboardDibToPng(const BITMAPINFOHEADER *pbih, SIZE_T cbDib,
                     void **ppvData, unsigned long *pcbData)
{
    void *pvBmp = NULL;
    unsigned long cbBmp = 0;
    unsigned char *pixels;
    unsigned char *png;
    int w, h, channels;
    int out_len = 0;

    /* Step 1: wrap DIB as a .bmp file in memory */
    if (!winClipboardDibToBmp(pbih, cbDib, &pvBmp, &cbBmp))
        return FALSE;

    /* Step 2: decode .bmp to RGBA pixels via stb_image */
    pixels = stbi_load_from_memory((const stbi_uc *) pvBmp, (int) cbBmp,
                                    &w, &h, &channels, 4);
    free(pvBmp);
    if (!pixels)
        return FALSE;

    /* Step 3: encode RGBA to PNG via stb_image_write */
    png = stbi_write_png_to_mem(pixels, w * 4, w, h, 4, &out_len);
    stbi_image_free(pixels);
    if (!png)
        return FALSE;

    *ppvData = png;
    *pcbData = (unsigned long) out_len;
    return TRUE;
}

/*
 * Memory-buffer context for stbi_write_jpg_to_func.
 * v1.16 of stb_image_write.h lacks stbi_write_jpg_to_mem, so we drive the
 * callback-based API ourselves.  A simple growing buffer that can be freed
 * with a single free() call.
 */
typedef struct {
    unsigned char *buf;
    int len;
    int cap;
} JpgMemContext;

static void
jpg_mem_write(void *context, void *data, int size)
{
    JpgMemContext *ctx = (JpgMemContext *) context;
    if (ctx->len + size > ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 4096;
        if (ctx->cap < ctx->len + size)
            ctx->cap = ctx->len + size;
        ctx->buf = realloc(ctx->buf, ctx->cap);
    }
    memcpy(ctx->buf + ctx->len, data, size);
    ctx->len += size;
}

/*
 * Re-encode a packed DIB to JPEG (image/jpeg).
 * Same BMP round-trip as winClipboardDibToPng: DIB → .bmp → stbi_load (RGB)
 * → stbi_write_jpg.  Quality defaults to 85; alpha is stripped (JPEG has no
 * alpha channel).
 */
static BOOL
winClipboardDibToJpeg(const BITMAPINFOHEADER *pbih, SIZE_T cbDib,
                      void **ppvData, unsigned long *pcbData)
{
    void *pvBmp = NULL;
    unsigned long cbBmp = 0;
    unsigned char *pixels;
    int w, h, channels;
    JpgMemContext ctx = { NULL, 0, 0 };

    if (!winClipboardDibToBmp(pbih, cbDib, &pvBmp, &cbBmp))
        return FALSE;

    /* Force 3 channels (RGB) — JPEG has no alpha */
    pixels = stbi_load_from_memory((const stbi_uc *) pvBmp, (int) cbBmp,
                                    &w, &h, &channels, 3);
    free(pvBmp);
    if (!pixels)
        return FALSE;

    if (0 == stbi_write_jpg_to_func(jpg_mem_write, &ctx, w, h, 3, pixels, 85)) {
        stbi_image_free(pixels);
        free(ctx.buf);
        return FALSE;
    }

    stbi_image_free(pixels);
    *ppvData = ctx.buf;
    *pcbData = (unsigned long) ctx.len;
    return TRUE;
}

/*
 * ---- GIF encoder / decoder ------------------------------------------------
 *
 * Uses giflib for GIF container handling and LZW codec; we provide the
 * DIB-to-indexed-pixel palette conversion.
 */

#include "gif_lib.h"

/* In-memory output buffer for giflib's OutputFunc callback. */
typedef struct {
    unsigned char *data;
    int pos;
    int cap;
} GifMemBuffer;

static int
gif_mem_write(GifFileType *gif, const GifByteType *buf, int len)
{
    GifMemBuffer *mb = (GifMemBuffer *) gif->UserData;
    int needed = mb->pos + len;

    if (needed > mb->cap) {
        int new_cap = mb->cap ? mb->cap : 4096;
        while (new_cap < needed)
            new_cap *= 2;
        {
            unsigned char *new_data = realloc(mb->data, (size_t) new_cap);
            if (!new_data)
                return 0;
            mb->data = new_data;
            mb->cap = new_cap;
        }
    }

    memcpy(mb->data + mb->pos, buf, (size_t) len);
    mb->pos += len;
    return len;
}

/* In-memory input buffer for giflib's InputFunc callback. */
typedef struct {
    const unsigned char *data;
    int pos;
    int len;
} GifMemReader;

static int
gif_mem_read(GifFileType *gif, GifByteType *buf, int len)
{
    GifMemReader *mr = (GifMemReader *) gif->UserData;
    int avail = mr->len - mr->pos;
    if (avail <= 0) return 0;
    if (len > avail) len = avail;
    memcpy(buf, mr->data + mr->pos, (size_t) len);
    mr->pos += len;
    return len;
}

/*
 * Declare GifQuantizeBuffer from giflib's quantize.c — not in gif_lib.h.
 */
extern int GifQuantizeBuffer(unsigned int Width, unsigned int Height,
                             int *ColorMapSize,
                             const GifByteType *RedInput,
                             const GifByteType *GreenInput,
                             const GifByteType *BlueInput,
                             GifByteType *OutputBuffer,
                             GifColorType *OutputColorMap);

/*
 * Encode a packed DIB as a complete GIF89a file using giflib for
 * colour quantisation (GifQuantizeBuffer), LZW compression, and
 * GIF container formatting.
 *
 * On success *ppvData is a malloc()'d buffer; on failure returns FALSE
 * having freed all intermediate allocations.
 */
static BOOL
winClipboardDibToGif(const BITMAPINFOHEADER *pbih, SIZE_T cbDib,
                     void **ppvData, unsigned long *pcbData)
{
    int w, h, bpp, stride, pix_offset, num_pixels;
    GifByteType *red = NULL, *green = NULL, *blue = NULL;
    GifByteType *indices = NULL;
    GifColorType colors[256];
    int cmap_size, gct_size, i, row, col;
    GifMemBuffer mb = { NULL, 0, 0 };
    GifFileType *gif = NULL;
    ColorMapObject *color_map = NULL;
    int error;
    BOOL ok = FALSE;

    w = pbih->biWidth;
    h = pbih->biHeight < 0 ? -(int) pbih->biHeight : (int) pbih->biHeight;
    if (w <= 0 || h <= 0 || w > 65535 || h > 65535)
        return FALSE;
    num_pixels = w * h;

    bpp = pbih->biBitCount;
    if (bpp != 24 && bpp != 32)
        return FALSE;

    pix_offset = (int) winClipboardDibPixelOffset(pbih);
    if (pix_offset <= 0 || pix_offset > (int) cbDib)
        return FALSE;
    stride = ((w * bpp + 31) / 32) * 4;

    /* Build planar R/G/B arrays from the DIB for GifQuantizeBuffer.
       DIB pixels are bottom-up BGRA; extract top-down RGB. */
    red   = malloc((size_t) num_pixels);
    green = malloc((size_t) num_pixels);
    blue  = malloc((size_t) num_pixels);
    indices = malloc((size_t) num_pixels);
    if (!red || !green || !blue || !indices)
        goto fail;

    {
        int bytes_pp = bpp / 8;
        const unsigned char *src = (const unsigned char *) pbih + pix_offset;
        for (row = 0; row < h; row++) {
            const unsigned char *src_row = src + (size_t) (h - 1 - row) * stride;
            GifByteType *r_row = red   + (size_t) row * w;
            GifByteType *g_row = green + (size_t) row * w;
            GifByteType *b_row = blue  + (size_t) row * w;
            for (col = 0; col < w; col++) {
                b_row[col] = src_row[col * bytes_pp];
                g_row[col] = src_row[col * bytes_pp + 1];
                r_row[col] = src_row[col * bytes_pp + 2];
            }
        }
    }

    /* Median-cut quantisation via giflib.  cmap_size is in/out. */
    cmap_size = 256;
    if (GifQuantizeBuffer((unsigned int) w, (unsigned int) h,
                          &cmap_size, red, green, blue,
                          indices, colors) == GIF_ERROR)
        goto fail;
    if (cmap_size < 2)
        cmap_size = 2;

    free(red);   red   = NULL;
    free(green); green = NULL;
    free(blue);  blue  = NULL;

    /* Round colour map to a power of 2 for the GIF global colour table. */
    gct_size = 2;
    while (gct_size < cmap_size)
        gct_size <<= 1;
    for (i = cmap_size; i < gct_size; i++)
        colors[i].Red = colors[i].Green = colors[i].Blue = 0;

    color_map = GifMakeMapObject(gct_size, colors);
    if (!color_map)
        goto fail;

    gif = EGifOpen(&mb, gif_mem_write, &error);
    if (!gif)
        goto fail;

    EGifSetGifVersion(gif, true);
    if (EGifPutScreenDesc(gif, w, h, 8 /* bits per primary */,
                          0 /* background index */, color_map) == GIF_ERROR)
        goto fail;

    GifFreeMapObject(color_map);
    color_map = NULL;

    /* Image descriptor: no local colour table (NULL = reuse global). */
    if (EGifPutImageDesc(gif, 0, 0, w, h, false, NULL) == GIF_ERROR)
        goto fail;

    /* GifQuantizeBuffer produces top-down indices, matching GIF row order. */
    for (row = 0; row < h; row++) {
        if (EGifPutLine(gif, indices + (size_t) row * w, w) == GIF_ERROR)
            goto fail;
    }

    if (EGifCloseFile(gif, &error) == GIF_ERROR)
        goto fail;
    gif = NULL;

    *ppvData = mb.data;
    *pcbData = (unsigned long) mb.pos;
    ok = TRUE;

fail:
    if (!ok)
        free(mb.data);
    free(indices);
    free(red);
    free(green);
    free(blue);
    if (color_map)
        GifFreeMapObject(color_map);
    if (gif)
        EGifCloseFile(gif, NULL);
    return ok;
}

/*
 * Produce the bytes for an image target atom from whatever image is currently
 * on the Win32 clipboard.
 */
BOOL
winClipboardEncodeImage(xcb_atom_t target, ClipboardAtoms *atoms,
                        void **ppvData, unsigned long *pcbData)
{
    HANDLE hDib;
    SIZE_T cbDib;
    BITMAPINFOHEADER *pbih;
    BOOL ok = FALSE;

    *ppvData = NULL;
    *pcbData = 0;

    if (target != atoms->atomImageBmp && target != atoms->atomImagePng
        && target != atoms->atomImageJpeg && target != atoms->atomImageGif)
        return FALSE;

    /* Windows synthesises CF_DIB from CF_BITMAP/CF_DIBV5 when needed. */
    hDib = GetClipboardData(CF_DIB);
    if (!hDib)
        return FALSE;

    cbDib = GlobalSize(hDib);
    if (cbDib < sizeof(BITMAPINFOHEADER))
        return FALSE;

    pbih = (BITMAPINFOHEADER *) GlobalLock(hDib);
    if (!pbih)
        return FALSE;

    if (pbih->biSize >= sizeof(BITMAPINFOHEADER)) {
        if (target == atoms->atomImagePng)
            ok = winClipboardDibToPng(pbih, cbDib, ppvData, pcbData);
        else if (target == atoms->atomImageJpeg)
            ok = winClipboardDibToJpeg(pbih, cbDib, ppvData, pcbData);
        else if (target == atoms->atomImageGif)
            ok = winClipboardDibToGif(pbih, cbDib, ppvData, pcbData);
        else
            ok = winClipboardDibToBmp(pbih, cbDib, ppvData, pcbData);
    }

    GlobalUnlock(hDib);
    return ok;
}

/*
 * Decode image data received from X11 (image/bmp, image/png, image/jpeg, image/gif)
 * back to a packed DIB suitable for SetClipboardData(CF_DIB, ...).
 *
 *   image/bmp  - already a .bmp file: strip the 14-byte BITMAPFILEHEADER
 *                and return the remaining DIB verbatim.
 *   image/png  - decode via stb_image to RGBA, build a 32-bit BI_RGB DIB.
 *   image/jpeg - same as png.
 *   image/gif  - decode via giflib, build a 32-bit BI_RGB DIB.
 *
 * On success *ppvDib is a malloc()'d buffer the caller must free();
 * on failure returns FALSE having allocated nothing.
 */
BOOL
winClipboardDecodeImageToDib(xcb_atom_t target, ClipboardAtoms *atoms,
                              const void *data, unsigned long len,
                              void **ppvDib, SIZE_T *pcbDib)
{
    *ppvDib = NULL;
    *pcbDib = 0;

    if (target == atoms->atomImageBmp) {
        /* Strip the 14-byte BITMAPFILEHEADER, validate the BM signature */
        const BITMAPFILEHEADER *pbfh = (const BITMAPFILEHEADER *) data;
        const unsigned char *src;
        unsigned char *dst;

        if (len < sizeof(BITMAPFILEHEADER))
            return FALSE;
        if (pbfh->bfType != 0x4D42)          /* 'BM' */
            return FALSE;

        src = (const unsigned char *) data + sizeof(BITMAPFILEHEADER);
        len -= sizeof(BITMAPFILEHEADER);
        if (len < sizeof(BITMAPINFOHEADER))
            return FALSE;

        dst = malloc(len);
        if (!dst)
            return FALSE;
        memcpy(dst, src, len);
        *ppvDib = dst;
        *pcbDib = (SIZE_T) len;
        return TRUE;
    }

    /* GIF: decode via giflib */
    if (target == atoms->atomImageGif) {
        GifMemReader mr;
        GifFileType *gif;
        int error;
        SavedImage *img;
        ColorMapObject *cmap;
        int w, h, transparent, i;
        BITMAPINFOHEADER *pbih;
        SIZE_T cbPixels, cbDib;
        unsigned char *dib;

        mr.data = (const unsigned char *) data;
        mr.pos = 0;
        mr.len = (int) len;

        gif = DGifOpen(&mr, gif_mem_read, &error);
        if (!gif) return FALSE;

        if (DGifSlurp(gif) == GIF_ERROR) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        if (gif->ImageCount < 1) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        img = &gif->SavedImages[0];
        w = img->ImageDesc.Width;
        h = img->ImageDesc.Height;
        if (w <= 0 || h <= 0 || w > 32767 || h > 32767) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        cmap = img->ImageDesc.ColorMap ? img->ImageDesc.ColorMap
                                       : gif->SColorMap;
        if (!cmap || cmap->ColorCount < 2) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        /* Check for transparency in Graphics Control Extension */
        transparent = NO_TRANSPARENT_COLOR;
        for (i = 0; i < img->ExtensionBlockCount; i++) {
            if (img->ExtensionBlocks[i].Function == GRAPHICS_EXT_FUNC_CODE) {
                GraphicsControlBlock gcb;
                if (DGifExtensionToGCB(img->ExtensionBlocks[i].ByteCount,
                                       img->ExtensionBlocks[i].Bytes,
                                       &gcb) == GIF_OK) {
                    transparent = gcb.TransparentColor;
                }
                break;
            }
        }

        /* Build 32-bit BI_RGB DIB (bottom-up BGRA) */
        cbPixels = (SIZE_T) w * (SIZE_T) h * 4;
        if (cbPixels > (SIZE_T) ULONG_MAX - sizeof(BITMAPINFOHEADER)) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        cbDib = sizeof(BITMAPINFOHEADER) + cbPixels;
        dib = malloc(cbDib);
        if (!dib) {
            DGifCloseFile(gif, &error);
            return FALSE;
        }

        pbih = (BITMAPINFOHEADER *) dib;
        memset(pbih, 0, sizeof(BITMAPINFOHEADER));
        pbih->biSize = sizeof(BITMAPINFOHEADER);
        pbih->biWidth = w;
        pbih->biHeight = h;
        pbih->biPlanes = 1;
        pbih->biBitCount = 32;
        pbih->biCompression = BI_RGB;
        pbih->biSizeImage = (DWORD) cbPixels;

        /* GIF is top-down; DIB is bottom-up. Flip rows while converting. */
        {
            int row;
            for (row = 0; row < h; row++) {
                unsigned char *dstRow = dib + sizeof(BITMAPINFOHEADER)
                    + (SIZE_T) (h - 1 - row) * w * 4;
                const unsigned char *srcRow = img->RasterBits
                    + (SIZE_T) row * w;
                int col;
                for (col = 0; col < w; col++) {
                    int idx = srcRow[col];
                    if (idx < 0 || idx >= cmap->ColorCount) idx = 0;
                    dstRow[col * 4 + 0] = cmap->Colors[idx].Blue;
                    dstRow[col * 4 + 1] = cmap->Colors[idx].Green;
                    dstRow[col * 4 + 2] = cmap->Colors[idx].Red;
                    dstRow[col * 4 + 3] = (idx == transparent) ? 0 : 255;
                }
            }
        }

        DGifCloseFile(gif, &error);
        *ppvDib = dib;
        *pcbDib = cbDib;
        return TRUE;
    }

    /* PNG / JPEG: decode via stb_image to RGBA, build a DIB */
    if (target == atoms->atomImagePng || target == atoms->atomImageJpeg) {
        int w, h, channels;
        unsigned char *pixels;
        BITMAPINFOHEADER *pbih;
        SIZE_T cbPixels, cbDib;
        unsigned char *dib;
        int row;

        pixels = stbi_load_from_memory((const stbi_uc *) data, (int) len,
                                       &w, &h, &channels, 4);
        if (!pixels)
            return FALSE;

        if (w <= 0 || h <= 0 || w > 32767 || h > 32767) {
            stbi_image_free(pixels);
            return FALSE;
        }

        /* 32-bit BI_RGB DIB: BITMAPINFOHEADER + pixel rows (bottom-up) */
        cbPixels = (SIZE_T) w * (SIZE_T) h * 4;
        if (cbPixels > (SIZE_T) ULONG_MAX - sizeof(BITMAPINFOHEADER)) {
            stbi_image_free(pixels);
            return FALSE;
        }

        cbDib = sizeof(BITMAPINFOHEADER) + cbPixels;
        dib = malloc(cbDib);
        if (!dib) {
            stbi_image_free(pixels);
            return FALSE;
        }

        pbih = (BITMAPINFOHEADER *) dib;
        memset(pbih, 0, sizeof(BITMAPINFOHEADER));
        pbih->biSize = sizeof(BITMAPINFOHEADER);
        pbih->biWidth = w;
        pbih->biHeight = h;             /* positive = bottom-up */
        pbih->biPlanes = 1;
        pbih->biBitCount = 32;
        pbih->biCompression = BI_RGB;
        pbih->biSizeImage = (DWORD) cbPixels;

        /* stb_image returns top-down RGBA; DIB expects bottom-up BGRA.
           Flip rows and swap R<->B in one pass. */
        {
            unsigned char *dstRow = dib + sizeof(BITMAPINFOHEADER)
                                    + (SIZE_T) (h - 1) * w * 4;
            const unsigned char *srcRow = pixels;
            for (row = 0; row < h; row++) {
                int col;
                for (col = 0; col < w; col++) {
                    dstRow[col * 4 + 0] = srcRow[col * 4 + 2];  /* B */
                    dstRow[col * 4 + 1] = srcRow[col * 4 + 1];  /* G */
                    dstRow[col * 4 + 2] = srcRow[col * 4 + 0];  /* R */
                    dstRow[col * 4 + 3] = srcRow[col * 4 + 3];  /* A */
                }
                srcRow += w * 4;
                dstRow -= w * 4;
            }
        }

        stbi_image_free(pixels);
        *ppvDib = dib;
        *pcbDib = cbDib;
        return TRUE;
    }

    return FALSE;
}
