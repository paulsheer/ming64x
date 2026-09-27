/* usertime.c — minimal millitime/millisleep for the coroutine library on
   MinGW-w64. The upstream usertime.c pulls in llrand/m51support, but corout.c
   only needs these two monotonic timer primitives. */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <time.h>

#include "usertime.h"

long long
millitime(void)
{
    return (long long)GetTickCount64();
}
