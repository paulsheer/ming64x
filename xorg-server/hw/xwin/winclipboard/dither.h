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
 * BSCQ color quantization with Floyd-Steinberg dithering for GIF output.
 *
 * Algorithm: M. Orchard and C. Bouman, "Color Quantization of Images,"
 * IEEE Trans. on Sig. Proc., vol. 39, no. 12, pp. 2677-2690, Dec. 1991.
 */

#ifndef WINCLIPBOARD_DITHER_H
#define WINCLIPBOARD_DITHER_H

/*
 * BSCQ color quantization with optional Floyd-Steinberg error diffusion.
 *
 * Replaces giflib's GifQuantizeBuffer (median cut, no dithering) with
 * perceptually superior binary-splitting palette design plus dithering.
 *
 * Input:
 *   rgb[width*height*3]  - top-down interleaved R,G,B bytes
 *   width, height         - image dimensions
 *   maxColors             - target palette size (2..256)
 *   doDither              - non-zero to apply Floyd-Steinberg dithering
 *
 * Output:
 *   colormapOut[maxColors*3] - palette (may have fewer than maxColors entries)
 *   indicesOut[width*height] - palette index per pixel
 *   outColors                - actual number of colors in the palette
 *
 * Returns 1 on success, 0 on failure.
 */
int bscqQuantize(const unsigned char *rgb, int width, int height,
                 int maxColors, int doDither,
                 unsigned char *colormapOut, unsigned char *indicesOut,
                 int *outColors);

#endif /* WINCLIPBOARD_DITHER_H */
