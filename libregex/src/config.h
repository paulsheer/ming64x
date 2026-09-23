/* libregex — minimal config for a Windows/MinGW single-byte, non-NLS build.
 * The regex engine defaults safely when gnulib feature macros are undefined;
 * these two are pinned so langinfo.h/nl_langinfo.c take their shim paths. */
#ifndef LIBREGEX_CONFIG_H
#define LIBREGEX_CONFIG_H

#define HAVE_LANGINFO_H 0
#define REPLACE_NL_LANGINFO 0

#endif /* LIBREGEX_CONFIG_H */
