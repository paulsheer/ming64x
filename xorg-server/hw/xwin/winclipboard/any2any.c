/*
 * Copyright (C) 2024 The VcXsrv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Any-to-any image format conversion for CF_HDROP clipboard operations.
 * Decodes source image bytes to RGBA, then re-encodes to the target format.
 * PNG/JPEG/BMP via stb_image / stb_image_write; GIF via giflib.
 */

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif

#include "internal.h"
#include "any2any.h"

#include <limits.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#include "stb_image.h"
#include "stb_image_write.h"
#include "gif_lib.h"
#include "dither.h"

/* --- internal image format enum ----------------------------------------- */

enum ImageFmt {
    FMT_PNG  = 0,
    FMT_BMP  = 1,
    FMT_JPEG = 2,
    FMT_GIF  = 3
};

/* --- giflib in-memory callbacks (per-TU copies of imageconv.c statics) -- */

typedef struct {
    unsigned char *data;
    int pos;
    int cap;
} GifMemBuffer;

static int
gifMemWrite(GifFileType *gif, const GifByteType *buf, int len)
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

typedef struct {
    const unsigned char *data;
    int pos;
    int len;
} GifMemReader;

static int
gifMemRead(GifFileType *gif, GifByteType *buf, int len)
{
    GifMemReader *mr = (GifMemReader *) gif->UserData;
    int avail = mr->len - mr->pos;
    if (avail <= 0) return 0;
    if (len > avail) len = avail;
    memcpy(buf, mr->data + mr->pos, (size_t) len);
    mr->pos += len;
    return len;
}

extern int GifQuantizeBuffer(unsigned int Width, unsigned int Height,
                              int *ColorMapSize,
                              const GifByteType *RedInput,
                              const GifByteType *GreenInput,
                              const GifByteType *BlueInput,
                              GifByteType *OutputBuffer,
                              GifColorType *OutputColorMap);

/* --- stb_image_write in-memory callback --------------------------------- */

typedef struct {
    unsigned char *buf;
    int len;
    int cap;
} MemBufContext;

static void
memBufWrite(void *context, void *data, int size)
{
    MemBufContext *ctx = (MemBufContext *) context;
    if (ctx->len + size > ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 4096;
        if (ctx->cap < ctx->len + size)
            ctx->cap = ctx->len + size;
        ctx->buf = realloc(ctx->buf, ctx->cap);
    }
    memcpy(ctx->buf + ctx->len, data, size);
    ctx->len += size;
}

/* --- decode source bytes to top-down RGBA ------------------------------- */

static unsigned char *
decodeToRgba(const void *src, unsigned long srcLen, int *w, int *h, int fmt)
{
    if (fmt == FMT_PNG || fmt == FMT_BMP || fmt == FMT_JPEG) {
        return stbi_load_from_memory((const stbi_uc *) src, (int) srcLen,
                                      w, h, NULL, 4);
    }

    /* FMT_GIF */
    {
        GifMemReader mr;
        GifFileType *gif;
        int error;
        SavedImage *img;
        ColorMapObject *cmap;
        int iw, ih, transparent, i;
        unsigned char *rgba;
        int row, col;

        mr.data = (const unsigned char *) src;
        mr.pos = 0;
        mr.len = (int) srcLen;

        gif = DGifOpen(&mr, gifMemRead, &error);
        if (!gif) return NULL;
        if (DGifSlurp(gif) == GIF_ERROR) {
            DGifCloseFile(gif, &error);
            return NULL;
        }
        if (gif->ImageCount < 1) {
            DGifCloseFile(gif, &error);
            return NULL;
        }

        img = &gif->SavedImages[0];
        iw = img->ImageDesc.Width;
        ih = img->ImageDesc.Height;
        if (iw <= 0 || ih <= 0) {
            DGifCloseFile(gif, &error);
            return NULL;
        }

        cmap = img->ImageDesc.ColorMap ? img->ImageDesc.ColorMap
                                       : gif->SColorMap;
        if (!cmap || cmap->ColorCount < 2) {
            DGifCloseFile(gif, &error);
            return NULL;
        }

        transparent = NO_TRANSPARENT_COLOR;
        for (i = 0; i < img->ExtensionBlockCount; i++) {
            if (img->ExtensionBlocks[i].Function == GRAPHICS_EXT_FUNC_CODE) {
                GraphicsControlBlock gcb;
                if (DGifExtensionToGCB(img->ExtensionBlocks[i].ByteCount,
                                       img->ExtensionBlocks[i].Bytes,
                                       &gcb) == GIF_OK)
                    transparent = gcb.TransparentColor;
                break;
            }
        }

        rgba = malloc((size_t) iw * (size_t) ih * 4);
        if (!rgba) {
            DGifCloseFile(gif, &error);
            return NULL;
        }

        /* GIF is top-down; produce top-down RGBA */
        for (row = 0; row < ih; row++) {
            const unsigned char *srcRow = img->RasterBits
                                          + (size_t) row * iw;
            unsigned char *dstRow = rgba + (size_t) row * iw * 4;
            for (col = 0; col < iw; col++) {
                int idx = srcRow[col];
                if (idx < 0 || idx >= cmap->ColorCount) idx = 0;
                dstRow[col * 4 + 0] = cmap->Colors[idx].Red;
                dstRow[col * 4 + 1] = cmap->Colors[idx].Green;
                dstRow[col * 4 + 2] = cmap->Colors[idx].Blue;
                dstRow[col * 4 + 3] = (idx == transparent) ? 0 : 255;
            }
        }

        DGifCloseFile(gif, &error);
        *w = iw;
        *h = ih;
        return rgba;
    }
}

/* --- encode RGBA to BMP file (BITMAPFILEHEADER + DIB) ------------------- */

static BOOL
rgbaToBmpFlatten(const unsigned char *rgba, int w, int h,
          void **ppvData, unsigned long *pcbData)
{
    SIZE_T cbPixels, cbFile;
    int stride, row;
    unsigned char *out;
    BITMAPFILEHEADER *pbfh;
    BITMAPINFOHEADER *pbih;

    if (w <= 0 || h <= 0)
        return FALSE;

    /* 24-bit DIB rows are DWORD-aligned */
    stride = ((w * 24 + 31) / 32) * 4;
    cbPixels = (SIZE_T) stride * (SIZE_T) h;
    if (cbPixels > (SIZE_T) ULONG_MAX - sizeof(BITMAPFILEHEADER)
                  - sizeof(BITMAPINFOHEADER))
        return FALSE;
    cbFile = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + cbPixels;

    out = malloc(cbFile);
    if (!out) return FALSE;

    /* BITMAPFILEHEADER */
    pbfh = (BITMAPFILEHEADER *) out;
    pbfh->bfType = 0x4D42;      /* 'BM' */
    pbfh->bfSize = (DWORD) cbFile;
    pbfh->bfReserved1 = 0;
    pbfh->bfReserved2 = 0;
    pbfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    /* BITMAPINFOHEADER */
    pbih = (BITMAPINFOHEADER *) (out + sizeof(BITMAPFILEHEADER));
    memset(pbih, 0, sizeof(BITMAPINFOHEADER));
    pbih->biSize = sizeof(BITMAPINFOHEADER);
    pbih->biWidth = w;
    pbih->biHeight = h;                 /* positive = bottom-up */
    pbih->biPlanes = 1;
    pbih->biBitCount = 24;
    pbih->biCompression = BI_RGB;
    pbih->biSizeImage = (DWORD) cbPixels;

    /* RGBA is top-down; DIB is bottom-up BGR.
       Composite alpha against gray (128,128,128) so feathered
       edges have a smooth transition rather than a hard cut. */
    {
        unsigned char *dstRow = out + pbfh->bfOffBits
                                + (SIZE_T) (h - 1) * stride;
        const unsigned char *srcRow = rgba;
        for (row = 0; row < h; row++) {
            int col;
            for (col = 0; col < w; col++) {
                unsigned int r = srcRow[col * 4 + 0];
                unsigned int g = srcRow[col * 4 + 1];
                unsigned int b = srcRow[col * 4 + 2];
                unsigned int a = srcRow[col * 4 + 3];
                unsigned int inv_a = 255 - a;
                dstRow[col * 3 + 0] =
                    (unsigned char)((b * a + 128 * inv_a) / 255);
                dstRow[col * 3 + 1] =
                    (unsigned char)((g * a + 128 * inv_a) / 255);
                dstRow[col * 3 + 2] =
                    (unsigned char)((r * a + 128 * inv_a) / 255);
            }
            /* Zero-fill row padding bytes (if stride > w*3) */
            for (col = w * 3; col < stride; col++)
                dstRow[col] = 0;
            srcRow += w * 4;
            dstRow -= stride;
        }
    }

    *ppvData = out;
    *pcbData = (unsigned long) cbFile;
    return TRUE;
}

static BOOL
rgbaToBmp(const unsigned char *rgba, int w, int h,
          void **ppvData, unsigned long *pcbData)
{
    SIZE_T cbPixels, cbFile;
    unsigned char *out;
    BITMAPFILEHEADER *pbfh;
    BITMAPINFOHEADER *pbih;
    int row;

    if (w <= 0 || h <= 0)
        return FALSE;

    cbPixels = (SIZE_T) w * (SIZE_T) h * 4;
    if (cbPixels > (SIZE_T) ULONG_MAX - sizeof(BITMAPFILEHEADER)
                  - sizeof(BITMAPINFOHEADER))
        return FALSE;
    cbFile = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + cbPixels;

    out = malloc(cbFile);
    if (!out) return FALSE;

    /* BITMAPFILEHEADER */
    pbfh = (BITMAPFILEHEADER *) out;
    pbfh->bfType = 0x4D42;      /* 'BM' */
    pbfh->bfSize = (DWORD) cbFile;
    pbfh->bfReserved1 = 0;
    pbfh->bfReserved2 = 0;
    pbfh->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    /* BITMAPINFOHEADER */
    pbih = (BITMAPINFOHEADER *) (out + sizeof(BITMAPFILEHEADER));
    memset(pbih, 0, sizeof(BITMAPINFOHEADER));
    pbih->biSize = sizeof(BITMAPINFOHEADER);
    pbih->biWidth = w;
    pbih->biHeight = h;                 /* positive = bottom-up */
    pbih->biPlanes = 1;
    pbih->biBitCount = 32;
    pbih->biCompression = BI_RGB;
    pbih->biSizeImage = (DWORD) cbPixels;

    /* RGBA is top-down; DIB is bottom-up BGRA. Flip rows, swap R<->B. */
    {
        unsigned char *dstRow = out + pbfh->bfOffBits
                                + (SIZE_T) (h - 1) * w * 4;
        const unsigned char *srcRow = rgba;
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

    *ppvData = out;
    *pcbData = (unsigned long) cbFile;
    return TRUE;
}

/* --- encode RGBA to GIF via giflib -------------------------------------- */

static BOOL
rgbaToGif(const unsigned char *rgba, int w, int h,
          void **ppvData, unsigned long *pcbData)
{
    int num_pixels, i, cmap_size, gct_size, row;
    int outColors, maxColors;
    unsigned char *rgb = NULL, *indices = NULL;
    unsigned char colormapOut[256 * 3];
    GifColorType colors[256];
    GifMemBuffer mb = { NULL, 0, 0 };
    GifFileType *gif = NULL;
    ColorMapObject *color_map = NULL;
    int error;
    BOOL ok = FALSE, hasTransparency;

    if (w <= 0 || h <= 0 || w > 65535 || h > 65535)
        return FALSE;
    num_pixels = w * h;

    rgb     = malloc((size_t) num_pixels * 3);
    indices = malloc((size_t) num_pixels);
    if (!rgb || !indices)
        goto fail;

    /* Build interleaved RGB; zero out fully-transparent pixels so they
       don't pollute the palette or bleed colour via error diffusion. */
    hasTransparency = FALSE;
    for (i = 0; i < num_pixels; i++) {
        if (rgba[i * 4 + 3] < 128) {
            rgb[i * 3 + 0] = 0;
            rgb[i * 3 + 1] = 0;
            rgb[i * 3 + 2] = 0;
            hasTransparency = TRUE;
        } else {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
    }

    /* Reserve palette entry 255 for transparency if needed */
    maxColors = hasTransparency ? 255 : 256;
    if (!bscqQuantize(rgb, w, h, maxColors, 1 /* dither */,
                       colormapOut, indices, &outColors))
        goto fail;
    if (outColors < 2) outColors = 2;

    free(rgb);  rgb = NULL;

    /* Unpack flat palette into giflib colour struct */
    for (i = 0; i < outColors; i++) {
        colors[i].Red   = colormapOut[i * 3 + 0];
        colors[i].Green = colormapOut[i * 3 + 1];
        colors[i].Blue  = colormapOut[i * 3 + 2];
    }

    if (hasTransparency) {
        int transIdx = 255;
        for (i = 0; i < num_pixels; i++)
            if (rgba[i * 4 + 3] < 128)
                indices[i] = (unsigned char) transIdx;

        colors[transIdx].Red   = 0;
        colors[transIdx].Green = 0;
        colors[transIdx].Blue  = 0;

        if (outColors <= transIdx)
            outColors = transIdx + 1;
        cmap_size = outColors;
    } else {
        cmap_size = outColors;
    }

    /* GIF colour table must be power-of-2 sized */
    gct_size = 2;
    while (gct_size < cmap_size) gct_size <<= 1;
    for (i = cmap_size; i < gct_size; i++)
        colors[i].Red = colors[i].Green = colors[i].Blue = 0;

    color_map = GifMakeMapObject(gct_size, colors);
    if (!color_map) goto fail;

    gif = EGifOpen(&mb, gifMemWrite, &error);
    if (!gif) goto fail;

    EGifSetGifVersion(gif, true);
    if (EGifPutScreenDesc(gif, w, h, 8, 0, color_map) == GIF_ERROR)
        goto fail;

    GifFreeMapObject(color_map);
    color_map = NULL;

    if (hasTransparency) {
        GifByteType gce[4];
        gce[0] = 0x01;
        gce[1] = 0x00;
        gce[2] = 0x00;
        gce[3] = 255;
        EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, gce);
    }

    if (EGifPutImageDesc(gif, 0, 0, w, h, false, NULL) == GIF_ERROR)
        goto fail;

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
    if (!ok) free(mb.data);
    free(indices);
    free(rgb);
    if (color_map) GifFreeMapObject(color_map);
    if (gif) EGifCloseFile(gif, NULL);
    return ok;
}

/* --- encode RGBA to target format --------------------------------------- */

static BOOL
encodeFromRgba(const unsigned char *rgba, int w, int h,
               void **ppvData, unsigned long *pcbData, int fmt)
{
    switch (fmt) {
    case FMT_PNG: {
        MemBufContext ctx = { NULL, 0, 0 };
        if (0 == stbi_write_png_to_func(memBufWrite, &ctx,
                                         w, h, 4, rgba, w * 4)) {
            free(ctx.buf);
            return FALSE;
        }
        *ppvData = ctx.buf;
        *pcbData = (unsigned long) ctx.len;
        return TRUE;
    }
    case FMT_JPEG: {
        MemBufContext ctx = { NULL, 0, 0 };
        unsigned char *rgb;
        int i, npixels;
        /* stbi_write_jpg_to_func has no stride parameter.
           Composite alpha against gray (128,128,128) so feathered
           edges have a smooth transition. */
        npixels = w * h;
        rgb = malloc((size_t) npixels * 3);
        if (!rgb) return FALSE;
        for (i = 0; i < npixels; i++) {
            unsigned int a = rgba[i * 4 + 3];
            unsigned int inv_a = 255 - a;
            rgb[i * 3 + 0] = (unsigned char)((rgba[i * 4 + 0] * a + 128 * inv_a) / 255);
            rgb[i * 3 + 1] = (unsigned char)((rgba[i * 4 + 1] * a + 128 * inv_a) / 255);
            rgb[i * 3 + 2] = (unsigned char)((rgba[i * 4 + 2] * a + 128 * inv_a) / 255);
        }
        if (0 == stbi_write_jpg_to_func(memBufWrite, &ctx,
                                         w, h, 3, rgb, 85)) {
            free(rgb);
            free(ctx.buf);
            return FALSE;
        }
        free(rgb);
        *ppvData = ctx.buf;
        *pcbData = (unsigned long) ctx.len;
        return TRUE;
    }
    case FMT_BMP:
        return rgbaToBmp(rgba, w, h, ppvData, pcbData);
    case FMT_GIF:
        return rgbaToGif(rgba, w, h, ppvData, pcbData);
    default:
        return FALSE;
    }
}

/* --- do a full decode → re-encode conversion ---------------------------- */

static BOOL
doConvert(void **ppvData, unsigned long *pcbData, int srcFmt, int dstFmt,
          char *statusmsg, size_t statusmsgSize)
{
    void *oldData = *ppvData;
    unsigned long oldLen = *pcbData;
    unsigned char *rgba;
    int w, h;

    rgba = decodeToRgba(oldData, oldLen, &w, &h, srcFmt);
    if (!rgba) {
        snprintf(statusmsg, statusmsgSize, "decode failed (fmt=%d, %lu bytes)",
                 srcFmt, (unsigned long) oldLen);
        return FALSE;
    }

    free(oldData);

    if (!encodeFromRgba(rgba, w, h, ppvData, pcbData, dstFmt)) {
        free(rgba);
        *ppvData = NULL;
        *pcbData = 0;
        snprintf(statusmsg, statusmsgSize,
                 "encode failed (%dx%d, fmt=%d)", w, h, dstFmt);
        return FALSE;
    }

    snprintf(statusmsg, statusmsgSize, "%dx%d fmt %d->%d, %lu bytes",
             w, h, srcFmt, dstFmt, (unsigned long) *pcbData);
    free(rgba);
    return TRUE;
}

/* --- identity (raw passthrough) ----------------------------------------- */

BOOL
convertFnRawToRaw(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    (void) ppvData;
    dbg_write("convertFnRawToRaw: entry cb=%lu",
              pcbData ? *pcbData : 0);
    snprintf(statusmsg, statusmsgSize, "no conversion");
    dbg_write("convertFnRawToRaw: OK (no conversion) cb=%lu",
              pcbData ? *pcbData : 0);
    return TRUE;
}

/* --- conversion functions ----------------------------------------------- */

BOOL
convertFnPngToGif(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnPngToGif: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_PNG, FMT_GIF, statusmsg, statusmsgSize);
    dbg_write("convertFnPngToGif: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnJpegToGif(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnJpegToGif: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_JPEG, FMT_GIF, statusmsg, statusmsgSize);
    dbg_write("convertFnJpegToGif: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnBmpToGif(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnBmpToGif: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_BMP, FMT_GIF, statusmsg, statusmsgSize);
    dbg_write("convertFnBmpToGif: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnGifToPng(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnGifToPng: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_GIF, FMT_PNG, statusmsg, statusmsgSize);
    dbg_write("convertFnGifToPng: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnJpegToPng(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnJpegToPng: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_JPEG, FMT_PNG, statusmsg, statusmsgSize);
    dbg_write("convertFnJpegToPng: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnBmpToPng(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnBmpToPng: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_BMP, FMT_PNG, statusmsg, statusmsgSize);
    dbg_write("convertFnBmpToPng: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnPngToJpeg(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnPngToJpeg: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_PNG, FMT_JPEG, statusmsg, statusmsgSize);
    dbg_write("convertFnPngToJpeg: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnGifToJpeg(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnGifToJpeg: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_GIF, FMT_JPEG, statusmsg, statusmsgSize);
    dbg_write("convertFnGifToJpeg: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnBmpToJpeg(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnBmpToJpeg: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_BMP, FMT_JPEG, statusmsg, statusmsgSize);
    dbg_write("convertFnBmpToJpeg: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnPngToBmp(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnPngToBmp: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_PNG, FMT_BMP, statusmsg, statusmsgSize);
    dbg_write("convertFnPngToBmp: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnGifToBmp(void **ppvData, unsigned long *pcbData,
                  char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnGifToBmp: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_GIF, FMT_BMP, statusmsg, statusmsgSize);
    dbg_write("convertFnGifToBmp: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}

BOOL
convertFnJpegToBmp(void **ppvData, unsigned long *pcbData,
                   char *statusmsg, size_t statusmsgSize)
{
    dbg_write("convertFnJpegToBmp: entry cb=%lu",
              pcbData ? *pcbData : 0);
    BOOL ok = doConvert(ppvData, pcbData, FMT_JPEG, FMT_BMP, statusmsg, statusmsgSize);
    dbg_write("convertFnJpegToBmp: %s cb=%lu",
              ok ? "OK" : "FAIL", pcbData ? *pcbData : 0);
    return ok;
}
