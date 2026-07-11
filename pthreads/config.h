/* config.h.  Generated from config.h.in by configure.  */
/* config.h.in.  Generated from configure.ac by autoheader.  */

#ifndef __PTW32_CONFIG_H
#define __PTW32_CONFIG_H

/* Define to 1 if you have the `calloc' function. */
#define HAVE_CALLOC 1

/* Define if CPU_AFFINITY is supported */
#define HAVE_CPU_AFFINITY 1

/* Define to 1 if you have the <errno.h> header file. */
#define HAVE_ERRNO_H 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define if your compiler knows about mode_t */
#define HAVE_MODE_T 1

/* Define to 1 if you have the <signal.h> header file. */
#define HAVE_SIGNAL_H 1

/* Define if your compiler knows about sigset_t */
/* #undef HAVE_SIGSET_T */

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define if your compiler knows about struct timespec */
#define HAVE_STRUCT_TIMESPEC 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <time.h> header file. */
#define HAVE_TIME_H 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the `_beginthreadex' function. */
#define HAVE__BEGINTHREADEX 1

/* Define if you do not have calloc */
/* #undef NEED_CALLOC */

/* Define if you do not have _beginthreadex */
/* #undef NEED_CREATETHREAD */

/* Define if DuplicateHandle is unsupported */
/* #undef NEED_DUPLICATEHANDLE */

/* Define if you do not have errno */
/* #undef NEED_ERRNO */

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT ""

/* Define to the full name of this package. */
#define PACKAGE_NAME "pthreads4w"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "pthreads4w git"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "pthreads4w"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "git"

/* Define to 1 if all of the C90 standard headers exist (not just the ones
   required in a freestanding environment). This macro is provided for
   backward compatibility; new code need not use it. */
#define STDC_HEADERS 1

/* Define to `__inline__' or `__inline' if that's what the C compiler
   calls it, or to nothing if 'inline' is not supported under any name.  */
#ifndef __cplusplus
/* #undef inline */
#endif

#define HAVE_C_INLINE

#endif
