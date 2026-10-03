/*
 *Copyright (C) 2001-2004 Harold L Hunt II All Rights Reserved.
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

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif

#include "os/osdep.h"

#include <../xfree86/common/xorgVersion.h>
#include "win.h"

#include "dix/input_priv.h"

/*
 * os/log.c:FatalError () calls our vendor ErrorF, so the message
 * from a FatalError will be logged.
 *
 * Attempt to do last-ditch, safe, important cleanup here.
 */
char g_FatalErrorMessage[1024];
void
OsVendorFatalError(const char *f, va_list args)
{
    /* Don't give duplicate warning if UseMsg was called */
    if (g_fSilentFatalError)
        return;

    if (!g_fLogInited) {
        g_fLogInited = TRUE;
        g_pszLogFile = LogInit(g_pszLogFile, ".old");
    }
    LogClose(EXIT_ERR_ABORT);

    /*
       Sometimes the error message needs a bit of cosmetic cleaning
       up for use in a dialog box...
     */
    {
        char *s;

        while ((s = strstr(g_FatalErrorMessage, "\n\t")) != NULL) {
            s[0] = ' ';
            s[1] = '\n';
        }
    }

    winMessageBoxF("A fatal error has occurred and " PROJECT_NAME " will now exit.\n\n"
                   "%s\n\n"
                   "Please open %s for more information.\n",
                   MB_ICONERROR,
                   g_FatalErrorMessage,
                   (g_pszLogFile ? g_pszLogFile : "the logfile"));
}

/*
 * winMessageBoxF - Print a formatted error message in a useful
 * message box.
 */

void
winMessageBoxF(const char *pszError, UINT uType, ...)
{
    char *pszErrorF = NULL;
    char *pszMsgBox = NULL;
    va_list args;
    int size;

    va_start(args, uType);
    size = vasprintf(&pszErrorF, pszError, args);
    va_end(args);
    if (size == -1) {
        pszErrorF = NULL;
        goto winMessageBoxF_Cleanup;
    }

#define MESSAGEBOXF \
	"%s\n" \
	"Vendor: %s\n" \
	"Release: %d.%d.%d.%d\n" \
	"Contact: %s\n" \
	"%s\n\n" \
	"XWin was started with the following command-line:\n\n" \
	"%s\n"

    size = asprintf(&pszMsgBox, MESSAGEBOXF,
                    pszErrorF, XVENDORNAME,
                    XORG_VERSION_MAJOR, XORG_VERSION_MINOR, XORG_VERSION_PATCH,
                    XORG_VERSION_SNAP,
                    BUILDERADDR, BUILDERSTRING, g_pszCommandLine);

    if (size == -1) {
        pszMsgBox = NULL;
        goto winMessageBoxF_Cleanup;
    }

    /* Display the message box. MB_USERICON with an explicit standard icon
       shows the icon without the automatic beep the MB_ICON* styles trigger. */
    {
        MSGBOXPARAMS mbp;
        const char *lpszIcon = NULL;

        switch (uType & MB_ICONMASK) {
        case MB_ICONERROR:
            lpszIcon = IDI_ERROR;
            break;
        case MB_ICONWARNING:
            lpszIcon = IDI_WARNING;
            break;
        case MB_ICONINFORMATION:
            lpszIcon = IDI_INFORMATION;
            break;
        case MB_ICONQUESTION:
            lpszIcon = IDI_QUESTION;
            break;
        }

        ZeroMemory(&mbp, sizeof(mbp));
        mbp.cbSize = sizeof(mbp);
        mbp.lpszText = pszMsgBox;
        mbp.lpszCaption = PROJECT_NAME;
        mbp.dwStyle = MB_OK | MB_TOPMOST;
        if (lpszIcon) {
            mbp.dwStyle |= MB_USERICON;
            mbp.lpszIcon = lpszIcon;
        }
        mbp.dwLanguageId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);

        MessageBoxIndirect(&mbp);
    }

 winMessageBoxF_Cleanup:
    free(pszErrorF);
    free(pszMsgBox);
#undef MESSAGEBOXF
}
