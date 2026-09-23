/* libsndfile — minimal config for a Windows/MinGW x86_64 static build.
 * Built-in codecs only: external FLAC/Ogg/Vorbis/Opus/Speex/MPEG are disabled
 * (flac.c/ogg.c/mpeg.c self-stub via their #else branches). */
#ifndef SNDFILE_CONFIG_H
#define SNDFILE_CONFIG_H

/* Package identity (sf_version_string() in sndfile.c string-concatenates). */
#define PACKAGE_NAME "libsndfile"
#define PACKAGE_VERSION "1.2.2"
#define PACKAGE_STRING "libsndfile 1.2.2"
#define PACKAGE_TARNAME "libsndfile"
#define PACKAGE_BUGREPORT "https://github.com/libsndfile/libsndfile/issues"
#define PACKAGE_URL "https://github.com/libsndfile/libsndfile"
#define VERSION "1.2.2"

/* Target: Windows x86_64, little-endian, GCC/Clang. */
#define COMPILER_IS_GCC 1
#define CPU_IS_LITTLE_ENDIAN 1
#define CPU_IS_BIG_ENDIAN 0
#define CPU_CLIPS_NEGATIVE 0
#define CPU_CLIPS_POSITIVE 0
#define OS_IS_WIN32 1
#define USE_WINDOWS_API 1

/* External codecs disabled. */
#define HAVE_EXTERNAL_XIPH_LIBS 0
#define HAVE_MPEG 0
#define ENABLE_EXPERIMENTAL_CODE 0

/* Headers present in MinGW-w64. */
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_IO_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_IMMINTRIN_H 1

/* C runtime functions present in MinGW-w64. */
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GMTIME 1

/* Type sizes used by common.h (sfwchar_t / BUF_UNION.lbuf). */
#define SIZEOF_WCHAR_T 2
#define SIZEOF_INT64_T 8
#define SIZEOF_OFF_T 8

#endif /* SNDFILE_CONFIG_H */
