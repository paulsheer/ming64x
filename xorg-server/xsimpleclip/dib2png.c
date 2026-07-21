/*
 * dib2png.c — Convert DIB/DIBV5 to PNG with transparency.
 *
 * Usage: ./dib2png <in.dib> <out.png>
 */
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "dib.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <in.dib> <out.png>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fprintf(stderr, "%s: empty or unreadable\n", argv[1]);
        fclose(fp);
        return 1;
    }
    unsigned char *data = (unsigned char *)malloc((size_t)sz);
    if (!data || fread(data, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "%s: read error\n", argv[1]);
        free(data);
        fclose(fp);
        return 1;
    }
    fclose(fp);

    int w, h;
    unsigned char *rgba = dibimg_load(data, (size_t)sz, &w, &h);
    free(data);
    if (!rgba) {
        fprintf(stderr, "%s: failed to decode DIB/DIBV5\n", argv[1]);
        return 1;
    }

    if (!stbi_write_png(argv[2], w, h, 4, rgba, w * 4)) {
        fprintf(stderr, "%s: failed to write PNG\n", argv[2]);
        free(rgba);
        return 1;
    }

    free(rgba);
    printf("%s: %dx%d DIB -> %s\n", argv[1], w, h, argv[2]);
    return 0;
}
