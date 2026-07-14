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
 */

#ifndef WINCLIPBOARD_ANY2ANY_H
#define WINCLIPBOARD_ANY2ANY_H

#include <X11/Xmd.h>           /* BOOL */
#include <stddef.h>             /* size_t */

typedef BOOL convertFn_t(void **ppvData, unsigned long *pcbData,
                         char *statusmsg, size_t statusmsgSize);

extern convertFn_t convertFnRawToRaw;

extern convertFn_t convertFnPngToGif;
extern convertFn_t convertFnJpegToGif;
extern convertFn_t convertFnBmpToGif;

extern convertFn_t convertFnGifToPng;
extern convertFn_t convertFnJpegToPng;
extern convertFn_t convertFnBmpToPng;

extern convertFn_t convertFnPngToJpeg;
extern convertFn_t convertFnGifToJpeg;
extern convertFn_t convertFnBmpToJpeg;

extern convertFn_t convertFnPngToBmp;
extern convertFn_t convertFnGifToBmp;
extern convertFn_t convertFnJpegToBmp;

#endif /* WINCLIPBOARD_ANY2ANY_H */
