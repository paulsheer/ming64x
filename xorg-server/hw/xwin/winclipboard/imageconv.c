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

/*
 * Minimal GDI+ flat-API declarations.  We deliberately avoid <gdiplus.h>
 * (a C++ header) so this remains C; we only declare the handful of entry
 * points we call.  WINAPI == __stdcall, which matches the gdiplus.dll exports.
 * GpStatus is an int enum where 0 (Ok) means success.
 */
typedef int GpStatus;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInputC;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token,
                               const GdiplusStartupInputC *input,
                               void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
GpStatus WINAPI GdipCreateBitmapFromGdiDib(const BITMAPINFO *gdiBitmapInfo,
                                           void *gdiBitmapData,
                                           void **bitmap);
GpStatus WINAPI GdipSaveImageToStream(void *image, IStream *stream,
                                      const CLSID *clsidEncoder,
                                      const void *encoderParams);
GpStatus WINAPI GdipDisposeImage(void *image);

/* Built-in GDI+ encoder CLSIDs (stable, documented values). */
static const CLSID CLSID_PngEncoder =
    { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const CLSID CLSID_JpegEncoder =
    { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

static ULONG_PTR g_gdiplusToken = 0;
static BOOL g_fGdiplusReady = FALSE;

/*
 * Start GDI+ for the lifetime of the clipboard thread.  Failure is non-fatal:
 * image/bmp (which needs no codec) and all text formats keep working; only
 * image/png and image/jpeg become unavailable.
 */
void
winClipboardImageInit(void)
{
    GdiplusStartupInputC input;

    input.GdiplusVersion = 1;
    input.DebugEventCallback = NULL;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = FALSE;

    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) == 0) {
        g_fGdiplusReady = TRUE;
        winDebug("winClipboardImageInit - GDI+ started, png/jpeg enabled\n");
    }
    else {
        g_gdiplusToken = 0;
        ErrorF("winClipboardImageInit - GdiplusStartup failed; "
               "image/png and image/jpeg clipboard support disabled\n");
    }
}

void
winClipboardImageShutdown(void)
{
    if (g_fGdiplusReady) {
        GdiplusShutdown(g_gdiplusToken);
        g_fGdiplusReady = FALSE;
        g_gdiplusToken = 0;
    }
}

/* Whether png/jpeg can be produced (i.e. GDI+ initialised). */
BOOL
winClipboardImageCodecsAvailable(void)
{
    return g_fGdiplusReady;
}

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
 * Re-encode a packed DIB to PNG or JPEG via GDI+.  GDI+ handles any colour
 * depth conversion internally; JPEG inherently discards the alpha channel.
 */
static BOOL
winClipboardDibToEncoded(const BITMAPINFOHEADER *pbih, SIZE_T cbDib,
                         const CLSID *clsidEncoder,
                         void **ppvData, unsigned long *pcbData)
{
    SIZE_T offBits;
    const BITMAPINFO *pbmi;
    void *pBits;
    void *pBitmap = NULL;
    IStream *pStream = NULL;
    HGLOBAL hMem = NULL;
    SIZE_T cbEncoded;
    void *pLocked;
    unsigned char *out;
    BOOL ok = FALSE;

    if (!g_fGdiplusReady)
        return FALSE;

    offBits = winClipboardDibPixelOffset(pbih);
    if (offBits > cbDib)
        return FALSE;

    pbmi = (const BITMAPINFO *) pbih;
    pBits = (unsigned char *) pbih + offBits;

    if (GdipCreateBitmapFromGdiDib(pbmi, pBits, &pBitmap) != 0 || !pBitmap)
        return FALSE;

    /* fDeleteOnRelease == TRUE: releasing the stream frees its HGLOBAL. */
    if (CreateStreamOnHGlobal(NULL, TRUE, &pStream) != S_OK || !pStream) {
        GdipDisposeImage(pBitmap);
        return FALSE;
    }

    if (GdipSaveImageToStream(pBitmap, pStream, clsidEncoder, NULL) != 0)
        goto cleanup;

    if (GetHGlobalFromStream(pStream, &hMem) != S_OK || !hMem)
        goto cleanup;

    cbEncoded = GlobalSize(hMem);
    if (cbEncoded == 0 || cbEncoded > (SIZE_T) ULONG_MAX)
        goto cleanup;                   /* nothing was written or it is too big */

    pLocked = GlobalLock(hMem);
    if (!pLocked)
        goto cleanup;

    out = malloc(cbEncoded);
    if (out) {
        memcpy(out, pLocked, cbEncoded);
        *ppvData = out;
        *pcbData = (unsigned long) cbEncoded;
        ok = TRUE;
    }
    GlobalUnlock(hMem);

 cleanup:
    /* Releasing the stream frees hMem (fDeleteOnRelease). */
    IStream_Release(pStream);
    GdipDisposeImage(pBitmap);
    return ok;
}

/*
 * Produce the bytes for an image target atom from whatever image is currently
 * on the Win32 clipboard.  The caller must already hold the clipboard open
 * (OpenClipboard) so GetClipboardData is valid.  On success *ppvData is a
 * malloc()'d buffer the caller must free(); on any failure it returns FALSE
 * having allocated nothing.
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

    /* Windows synthesises CF_DIB from CF_BITMAP/CF_DIBV5 when needed. */
    hDib = GetClipboardData(CF_DIB);
    if (!hDib)
        return FALSE;                   /* no image actually on the clipboard */

    cbDib = GlobalSize(hDib);
    if (cbDib < sizeof(BITMAPINFOHEADER))
        return FALSE;                   /* empty or zero-size image data */

    pbih = (BITMAPINFOHEADER *) GlobalLock(hDib);
    if (!pbih)
        return FALSE;

    if (pbih->biSize >= sizeof(BITMAPINFOHEADER)) {
        if (target == atoms->atomImageBmp)
            ok = winClipboardDibToBmp(pbih, cbDib, ppvData, pcbData);
        else if (target == atoms->atomImagePng)
            ok = winClipboardDibToEncoded(pbih, cbDib, &CLSID_PngEncoder,
                                          ppvData, pcbData);
        else if (target == atoms->atomImageJpeg)
            ok = winClipboardDibToEncoded(pbih, cbDib, &CLSID_JpegEncoder,
                                          ppvData, pcbData);
    }

    GlobalUnlock(hDib);
    return ok;
}
