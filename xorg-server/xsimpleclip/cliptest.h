/*
 * cliptest.h — Shared protocol header for clipboard test harness.
 *
 * Defines the wire format, message types, and test parameter structures
 * used by both win32cliptest.exe (Windows) and x11cliptest (Linux).
 */
#ifndef CLIPTEST_H
#define CLIPTEST_H

#include <stdint.h>

/* ---------- message types ---------- */
enum message_type {
    MSG_REQUEST  = 1,
    MSG_RESPONSE = 2,
    MSG_QUIT     = 3,
};

/* ---------- test direction ---------- */
enum direction {
    CLIPTEST_WIN32_TO_X11 = 1,
    CLIPTEST_X11_TO_WIN32 = 2,
};

/* ---------- image formats ---------- */
enum format {
    CLIPTEST_FORMAT_PNG  = 1,
    CLIPTEST_FORMAT_BMP  = 2,
    CLIPTEST_FORMAT_GIF  = 3,
    CLIPTEST_FORMAT_JPEG = 4,
    CLIPTEST_FORMAT_DIB  = 5,
    CLIPTEST_FORMAT_DIBV5 = 6,
};

/* ---------- transparency ---------- */
enum transparency {
    CLIPTEST_TRANSPARENCY_YES = 0,
    CLIPTEST_TRANSPARENCY_NO  = 1,
};

/* ---------- HDROP ---------- */
enum hdrop {
    CLIPTEST_HDROP_YES = 0,
    CLIPTEST_HDROP_NO  = 1,
};

/* ---------- test parameters (28 bytes packed) ---------- */
struct cliptest {
    uint32_t direction;
    uint32_t from_format;
    uint32_t to_format;
    uint32_t hdrop;
    int32_t  width;
    int32_t  height;
    uint32_t transparency;
};

/* ---------- wire message layouts ---------- */

/*
 * Request:  Linux → Win32
 *   [ type:u32 ][ length:u32 ][ index:u32 ]
 *   [ cliptest struct: 28 bytes ]
 *   [ image data: variable ]        — only when direction == X11_TO_WIN32
 */
struct message_request {
    uint32_t type;
    uint32_t length;
    uint32_t index;
    /* struct cliptest params follows */
    /* image data follows (optional) */
};

/*
 * Response: Win32 → Linux
 *   [ type:u32 ][ length:u32 ][ index:u32 ][ success:u32 ]
 *   [ ext:char[8] ][ image data: variable ]
 */
struct message_response {
    uint32_t type;
    uint32_t length;
    uint32_t index;
    uint32_t success;
    char ext[8];
    /* image data follows */
};

union message {
    struct message_request  request;
    struct message_response response;
    struct {
        uint32_t type;
        uint32_t length;
        uint32_t index;
    } generic;
};

/* ---------- wire sizes ---------- */
#define CLIPTEST_HEADER_SIZE     12   /* type + length + index */
#define CLIPTEST_STRUCT_SIZE     28   /* 7 * sizeof(uint32_t) */
#define CLIPTEST_REQUEST_BASE    40   /* HEADER + STRUCT */
#define CLIPTEST_RESPONSE_BASE   24   /* HEADER + success + ext[8] */

/* ---------- format helpers ---------- */
static inline const char *format_name(uint32_t fmt)
{
    switch (fmt) {
    case CLIPTEST_FORMAT_PNG:   return "png";
    case CLIPTEST_FORMAT_BMP:   return "bmp";
    case CLIPTEST_FORMAT_GIF:   return "gif";
    case CLIPTEST_FORMAT_JPEG:  return "jpeg";
    case CLIPTEST_FORMAT_DIB:   return "dib";
    case CLIPTEST_FORMAT_DIBV5: return "dibv5";
    default:                    return "unknown";
    }
}

static inline const char *format_ext(uint32_t fmt)
{
    switch (fmt) {
    case CLIPTEST_FORMAT_PNG:   return "png";
    case CLIPTEST_FORMAT_BMP:   return "bmp";
    case CLIPTEST_FORMAT_GIF:   return "gif";
    case CLIPTEST_FORMAT_JPEG:  return "jpg";
    case CLIPTEST_FORMAT_DIB:   return "dib";
    case CLIPTEST_FORMAT_DIBV5: return "dibv5";
    default:                    return "dat";
    }
}

static inline const char *direction_name(uint32_t dir)
{
    switch (dir) {
    case CLIPTEST_WIN32_TO_X11: return "WIN32_TO_X11";
    case CLIPTEST_X11_TO_WIN32: return "X11_TO_WIN32";
    default:                    return "UNKNOWN";
    }
}

static inline const char *transparency_name(uint32_t t)
{
    switch (t) {
    case CLIPTEST_TRANSPARENCY_YES: return "yes";
    case CLIPTEST_TRANSPARENCY_NO:  return "no";
    default:                        return "unknown";
    }
}

static inline const char *hdrop_name(uint32_t h)
{
    switch (h) {
    case CLIPTEST_HDROP_YES: return "yes";
    case CLIPTEST_HDROP_NO:  return "no";
    default:                 return "unknown";
    }
}

static inline const char *size_name(int width)
{
    if (width <= 200) return "small";
    if (width <= 900) return "medium";
    return "large";
}

#endif /* CLIPTEST_H */
