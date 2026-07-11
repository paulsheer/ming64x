#ifndef __GLHEADER_H__
#define __GLHEADER_H__

#define STDC_HEADERS 1

#include <X11/Xwinsock.h>
#include <X11/Xwindows.h>
#include <assert.h>
#define strcasecmp _stricmp

#undef MINSHORT
#undef MAXSHORT

#define MINSHORT -32768
#define MAXSHORT 32767

#ifndef PUBLIC
#define PUBLIC
#endif

#ifndef GLAPIENTRY
#define GLAPIENTRY APIENTRY
#endif
#ifndef GLAPIENTRYP
#define GLAPIENTRYP APIENTRY *
#endif

/* Mesa-specific packed depth/stencil formats (not in MinGW's GL/gl.h) */
#ifndef GL_DEPTH_STENCIL_MESA
#define GL_DEPTH_STENCIL_MESA			0x8750
#define GL_UNSIGNED_INT_24_8_MESA		0x8751
#define GL_UNSIGNED_INT_8_24_REV_MESA		0x8752
#define GL_UNSIGNED_SHORT_15_1_MESA		0x8753
#define GL_UNSIGNED_SHORT_1_15_REV_MESA		0x8754
#endif

/* Function pointer typedefs for extensions not in MinGW's glext.h */
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef PFNGLFRAMEBUFFERPARAMETERIMESAPROC
typedef void (APIENTRYP PFNGLFRAMEBUFFERPARAMETERIMESAPROC)(unsigned int target, unsigned int pname, int param);
typedef void (APIENTRYP PFNGLGETFRAMEBUFFERPARAMETERIVMESAPROC)(unsigned int target, unsigned int pname, int *params);
typedef void (APIENTRYP PFNGLNAMEDFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)(unsigned int framebuffer, unsigned int attachment, unsigned int texture, int level, int baseViewIndex, int numViews);
#endif

#define GL_GLEXT_PROTOTYPES
#ifdef __cplusplus
extern "C" {
#endif


/**
 * GL_FIXED is defined in glext.h version 64 but these typedefs aren't (yet).
 */
typedef int GLclampx;

#ifdef __cplusplus
}
#endif

#endif
