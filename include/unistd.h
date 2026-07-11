#ifndef __UNISTD_H__
#define __UNISTD_H__

#define strcasecmp _stricmp
#define strdup _strdup

#include <X11/Xw32defs.h>

#ifdef __MINGW32__
#include <io.h>
#include <process.h>
#endif
#endif
