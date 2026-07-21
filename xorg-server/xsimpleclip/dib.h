/*
 * dib.h — DIB / DIBV5 loader, stb_image-style API.
 */
#ifndef DIB_H
#define DIB_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load a DIB or DIBV5 image from memory, return top-down RGBA pixels.
 * Returns NULL on failure.  Caller must free() the returned buffer.
 */
unsigned char *dibimg_load(const unsigned char *data, size_t len,
                           int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* DIB_H */
