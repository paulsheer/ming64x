/*
 *Copyright (C) 2003-2004 Harold L Hunt II All Rights Reserved.
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
 *NONINFRINGEMENT. IN NO EVENT SHALL HAROLD L HUNT II BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *Except as contained in this notice, the name of Harold L Hunt II
 *shall not be used in advertising or otherwise to promote the sale, use
 *or other dealings in this Software without prior written authorization
 *from Harold L Hunt II.
 *
 * Authors:	Harold L Hunt II
 */

#ifndef WINCLIPBOARD_INTERNAL_H
#define WINCLIPBOARD_INTERNAL_H

/* Standard library headers */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#ifdef __CYGWIN__
#include <sys/select.h>
#else
#include <X11/Xwinsock.h>
#endif
#include <fcntl.h>
#include <setjmp.h>
#ifdef _MSC_VER
typedef int pid_t;
#endif
#include <pthread.h>

#include <xcb/xproto.h>
#include <X11/Xfuncproto.h> // for _X_ATTRIBUTE_PRINTF
#include <X11/Xmd.h> // for BOOL

/* Windows headers */
#include <X11/Xwindows.h>

#include "winmsg.h"

#define WIN_XEVENTS_SUCCESS			0  // more like 'CONTINUE'
#define WIN_XEVENTS_FAILED			1
#define WIN_XEVENTS_NOTIFY_DATA			3
#define WIN_XEVENTS_NOTIFY_TARGETS		4

#define WM_WM_QUIT                             (WM_USER + 201)

#define ARRAY_SIZE(a)  (sizeof((a)) / sizeof((a)[0]))

/*
 * References to external symbols
 */

extern char *display;
/*
 * winclipboardinit.c
 */

Bool
 winInitClipboard(void);

/*
 * winclipboardtextconv.c
 */

void
 winClipboardDOStoUNIX(char *pszData, int iLength);

void
 winClipboardUNIXtoDOS(char **ppszData, int iLength);

/*
 * imageconv.c
 */

typedef struct
{
    xcb_atom_t atomClipboard;
    xcb_atom_t atomLocalProperty;
    xcb_atom_t atomUTF8String;
    xcb_atom_t atomCompoundText;
    xcb_atom_t atomTargets;
    xcb_atom_t atomIncr;
    /* Image clipboard target atoms (Win32 clipboard -> X11 selection) */
    xcb_atom_t atomImagePng;
    xcb_atom_t atomImageBmp;
    xcb_atom_t atomImageJpeg;
    xcb_atom_t atomImageGif;
    xcb_atom_t atomImageProbe;   /* dedicated property for async TARGETS probe */
#ifdef CLIPDEBUG
    xcb_atom_t atomDebugOn;      /* CLIPTEST_DBG_ON — starts debug logging */
    xcb_atom_t atomDebugOff;     /* CLIPTEST_DBG_OFF — stops debug logging */
#endif
    /* Registered Win32 clipboard format IDs (populated at startup) */
    UINT cfPng;
    UINT cfJfif;
    UINT cfGif;
} ClipboardAtoms;

#ifdef CLIPDEBUG
/* Debug logging (xevents.c, wndproc.c) */
void dbg_open(void);
void dbg_write(const char *fmt, ...) _X_ATTRIBUTE_PRINTF(1, 2);
#else
#define dbg_write(fmt...)       do { } while(0)
#endif

/* Encode the Win32 clipboard image (CF_DIB) into the requested image target
   (image/bmp only — no PNG/JPEG encoder is linked).
   Caller must hold the clipboard open;
   on success *ppvData is a malloc()'d buffer the caller frees. */
BOOL
 winClipboardEncodeImage(xcb_atom_t target, ClipboardAtoms *atoms,
                         void **ppvData, unsigned long *pcbData);

/* Convert raw GIF bytes to PNG via stb_image decode + stb_image_write
   encode.  On success *ppvData is malloc()'d, caller frees. */
BOOL
 winClipboardGifToPng(const void *gifData, unsigned long gifLen,
                      void **ppvData, unsigned long *pcbData);

/* Decode X11 image data (image/bmp, image/png, image/jpeg) back to a
   packed DIB (BITMAPINFOHEADER + pixel data) for SetClipboardData(CF_DIB).
   PNG and JPEG are decoded via stb_image.
   On success *ppvDib is a malloc()'d buffer the caller frees. */
BOOL
 winClipboardDecodeImageToDib(xcb_atom_t target, ClipboardAtoms *atoms,
                               const void *data, unsigned long len,
                               void **ppvDib, SIZE_T *pcbDib,
                               BOOL fV5);

/*
 * winclipboardwndproc.c
 */

BOOL winClipboardFlushWindowsMessageQueue(HWND hwnd);

LRESULT CALLBACK
winClipboardWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

typedef struct
{
  xcb_connection_t *pClipboardDisplay;
  xcb_window_t iClipboardWindow;
  ClipboardAtoms *atoms;
} ClipboardWindowCreationParams;

/*
 * winclipboardxevents.c
 */

typedef struct
{
  xcb_atom_t *targetList;
  unsigned char *incr;
  unsigned long int incrsize;
  UINT requestedFmt;
} ClipboardConversionData;

int
winClipboardFlushXEvents(HWND hwnd,
                         xcb_window_t iWindow, xcb_connection_t * pDisplay,
                         ClipboardConversionData *data, ClipboardAtoms *atoms);

xcb_atom_t
winClipboardGetLastOwnedSelectionAtom(ClipboardAtoms *atoms);

void
winClipboardInitMonitoredSelections(void);

#endif
