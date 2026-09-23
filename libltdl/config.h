/* libltdl — minimal config for a Windows/MinGW static build (libtool 2.5.4). */
#ifndef LTDL_CONFIG_H
#define LTDL_CONFIG_H

/* error_t is a glibc/gnulib type; MinGW does not define it. */
#define error_t int

/* MinGW ships <unistd.h> (wraps <io.h>/<process.h>); access/close/read live there. */
#define HAVE_UNISTD_H 1

/* Windows static-library conventions. */
#define LT_OBJDIR ""
#define LT_MODULE_EXT ".dll"
#define LT_SHARED_EXT "dll"
#define LT_MODULE_PATH_VAR "PATH"

/* Deliberately left undefined so libltdl's `#ifndef HAVE_*` checks fall back
 * to the bundled implementations: HAVE_ARGZ_H, HAVE_WORKING_ARGZ,
 * HAVE_DIRENT_H, HAVE_STRLCAT, HAVE_STRLCPY, HAVE_DLFCN_H, HAVE_LIBDL,
 * HAVE_DECL_CYGWIN_CONV_PATH. LT_LIBEXT/LT_LIBPREFIX also stay undefined to
 * use their built-in "a"/"lib" defaults. */

#endif /* LTDL_CONFIG_H */
