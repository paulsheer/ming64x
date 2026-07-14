/*
 * Copyright (C) 2024 The VcXsrv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * BSCQ palette design + Floyd-Steinberg dithering.
 *
 * Based on "Binary Splitting Color Quantization" by M. Orchard and
 * C. Bouman, IEEE Trans. Sig. Proc., Dec. 1991.  Original reference
 * implementation from https://engineering.purdue.edu/~bouman/software/
 * color_quantization/ — ported from standalone TIFF tool to reusable C
 * library and extended with error-diffusion dithering.
 */

/*
 *  Binary Splitting Color Quantization software
 *   Charles A. Bouman
 *
 *     https://engineering.purdue.edu/~bouman/software/color_quantization/  
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dither.h"

enum {
    BSCQ_MAX_COLORS = 256,
    BSCQ_MAX_NODES  = 511    /* 2 * BSCQ_MAX_COLORS - 1, 1-based */
};

/* Per-node arrays for the BSCQ binary tree (index 0 unused). */
typedef struct {
    int     num_nodes;
    double  m     [BSCQ_MAX_NODES + 1][3];   /* sum */
    double  R     [BSCQ_MAX_NODES + 1][9];   /* autocorrelation */
    double  Rhat  [BSCQ_MAX_NODES + 1][9];   /* covariance */
    double  q     [BSCQ_MAX_NODES + 1][3];   /* centroid */
    double  cnt   [BSCQ_MAX_NODES + 1];      /* pixel count */
    double  lambda[BSCQ_MAX_NODES + 1];      /* largest eigenvalue */
    double  evec  [BSCQ_MAX_NODES + 1][3];   /* principal eigenvector */
    int     leaf  [BSCQ_MAX_NODES + 1];      /* 1 = leaf */
    int     child [BSCQ_MAX_NODES + 1][2];   /* left(0) / right(1) children */
    int     eigen_ok[BSCQ_MAX_NODES + 1];    /* eigenvalue cached? */
} BscqTree;

/* ======================================================================
 * Jacobi eigenvalue routine for 3x3 real symmetric matrix
 * ====================================================================== */

static int
eejcb_3(double a[9], double v[9], double eps, int jt)
{
    int i, j, p, q, u, w, t, s, l;
    double fm, cn, sn, omega, x, y;

    for (i = 0; i < 3; i++) {
        v[i * 3 + i] = 1.0;
        for (j = 0; j < 3; j++)
            if (i != j) v[i * 3 + j] = 0.0;
    }

    for (l = 1; l <= jt; l++) {
        fm = 0.0;  p = 0;  q = 1;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                double d = fabs(a[i * 3 + j]);
                if (i != j && d > fm) { fm = d; p = i; q = j; }
            }
        }
        if (fm < eps) return 1;

        u = p * 3 + q;  w = p * 3 + p;
        t = q * 3 + p;  s = q * 3 + q;

        x = -a[u];  y = (a[s] - a[w]) / 2.0;
        omega = x / sqrt(x * x + y * y);
        if (y < 0.0) omega = -omega;
        sn = 1.0 + sqrt(1.0 - omega * omega);
        sn = omega / sqrt(2.0 * sn);
        cn = sqrt(1.0 - sn * sn);

        fm = a[w];
        a[w] = fm * cn * cn + a[s] * sn * sn + a[u] * omega;
        a[s] = fm * sn * sn + a[s] * cn * cn - a[u] * omega;
        a[u] = 0.0;  a[t] = 0.0;

        for (j = 0; j < 3; j++) {
            if (j != p && j != q) {
                u = p * 3 + j;  w = q * 3 + j;
                fm = a[u];
                a[u] = fm * cn + a[w] * sn;
                a[w] = -fm * sn + a[w] * cn;
            }
        }
        for (i = 0; i < 3; i++) {
            if (i != p && i != q) {
                u = i * 3 + p;  w = i * 3 + q;
                fm = a[u];
                a[u] = fm * cn + a[w] * sn;
                a[w] = -fm * sn + a[w] * cn;
            }
        }
        for (i = 0; i < 3; i++) {
            u = i * 3 + p;  w = i * 3 + q;
            fm = v[u];
            v[u] = fm * cn + v[w] * sn;
            v[w] = -fm * sn + v[w] * cn;
        }
    }
    return 0;
}

static int
eigen_3(double a[9], double v[9], double eps, int jt)
{
    int i, mi;
    double temp;

    if (!eejcb_3(a, v, eps, jt)) return 0;

    /* move largest eigenvalue + its vector to first position */
    mi = 0;
    for (i = 1; i < 3; i++)
        if (a[i * 3 + i] > a[mi * 3 + mi]) mi = i;

    if (mi != 0) {
        temp = a[0];  a[0] = a[mi * 3 + mi];  a[mi * 3 + mi] = temp;
        for (i = 0; i < 3; i++) {
            temp = v[i * 3];
            v[i * 3] = v[i * 3 + mi];
            v[i * 3 + mi] = temp;
        }
    }
    return 1;
}

/* ======================================================================
 * BSCQ per-node stats
 * ====================================================================== */

static void
bscq_compute_node(BscqTree *t, int k)
{
    double inv;

    if (t->cnt[k] < 1.0) {
        t->q[k][0] = t->q[k][1] = t->q[k][2] = 0.0;
        memset(t->Rhat[k], 0, 9 * sizeof(double));
        t->lambda[k] = -1e100;
        t->eigen_ok[k] = 0;
        return;
    }

    inv = 1.0 / t->cnt[k];
    t->q[k][0] = t->m[k][0] * inv;
    t->q[k][1] = t->m[k][1] * inv;
    t->q[k][2] = t->m[k][2] * inv;

    /* Rhat = R - m*m'/N  (covariance matrix) */
    t->Rhat[k][0] = t->R[k][0] - t->m[k][0] * t->m[k][0] * inv;
    t->Rhat[k][1] = t->R[k][1] - t->m[k][0] * t->m[k][1] * inv;
    t->Rhat[k][2] = t->R[k][2] - t->m[k][0] * t->m[k][2] * inv;
    t->Rhat[k][3] = t->Rhat[k][1];  /* symmetric */
    t->Rhat[k][4] = t->R[k][4] - t->m[k][1] * t->m[k][1] * inv;
    t->Rhat[k][5] = t->R[k][5] - t->m[k][1] * t->m[k][2] * inv;
    t->Rhat[k][6] = t->Rhat[k][2];  /* symmetric */
    t->Rhat[k][7] = t->Rhat[k][5];  /* symmetric */
    t->Rhat[k][8] = t->R[k][8] - t->m[k][2] * t->m[k][2] * inv;

    t->eigen_ok[k] = 0;
}

static void
bscq_compute_eigen(BscqTree *t, int k)
{
    double a[9], v[9];

    if (t->eigen_ok[k]) return;

    memcpy(a, t->Rhat[k], 9 * sizeof(double));
    if (eigen_3(a, v, 1e-16, 1000000)) {
        t->lambda[k] = a[0];
        t->evec[k][0] = v[0];
        t->evec[k][1] = v[1];
        t->evec[k][2] = v[2];
    } else {
        t->lambda[k] = -1e100;
    }
    t->eigen_ok[k] = 1;
}

/* ======================================================================
 * BSCQ: split leaf node k into two children (rescan approach)
 * ====================================================================== */

static int
bscq_split(BscqTree *t, const unsigned char *rgb,
           int width, int height, uint16_t *cluster, int k)
{
    int n, left, right, npixels = width * height;
    double ev[3], eq, exs;

    if (!t->eigen_ok[k]) bscq_compute_eigen(t, k);

    ev[0] = t->evec[k][0];
    ev[1] = t->evec[k][1];
    ev[2] = t->evec[k][2];
    eq = t->q[k][0] * ev[0] + t->q[k][1] * ev[1] + t->q[k][2] * ev[2];

    left  = t->num_nodes + 1;
    right = left + 1;
    if (right >= BSCQ_MAX_NODES + 1) return 0;

    memset(t->m[left],  0, 3 * sizeof(double));
    memset(t->m[right], 0, 3 * sizeof(double));
    memset(t->R[left],  0, 9 * sizeof(double));
    memset(t->R[right], 0, 9 * sizeof(double));
    t->cnt[left]  = 0.0;
    t->cnt[right] = 0.0;
    t->leaf[left]  = 0;  t->leaf[right]  = 0;
    t->eigen_ok[left]  = 0;  t->eigen_ok[right]  = 0;
    t->child[left][0]  = -1;  t->child[left][1]  = -1;
    t->child[right][0] = -1;  t->child[right][1] = -1;

    for (n = 0; n < npixels; n++) {
        int pix;
        if (cluster[n] != k) continue;

        pix = n * 3;
        exs = (double)rgb[pix + 0] * ev[0]
            + (double)rgb[pix + 1] * ev[1]
            + (double)rgb[pix + 2] * ev[2];

        if (exs <= eq) {
            cluster[n] = (uint16_t) left;
            t->cnt[left]  += 1.0;
            t->m[left][0] += rgb[pix + 0];
            t->m[left][1] += rgb[pix + 1];
            t->m[left][2] += rgb[pix + 2];
            t->R[left][0] += (double)rgb[pix + 0] * rgb[pix + 0];
            t->R[left][1] += (double)rgb[pix + 0] * rgb[pix + 1];
            t->R[left][2] += (double)rgb[pix + 0] * rgb[pix + 2];
            t->R[left][4] += (double)rgb[pix + 1] * rgb[pix + 1];
            t->R[left][5] += (double)rgb[pix + 1] * rgb[pix + 2];
            t->R[left][8] += (double)rgb[pix + 2] * rgb[pix + 2];
        } else {
            cluster[n] = (uint16_t) right;
            t->cnt[right]  += 1.0;
            t->m[right][0] += rgb[pix + 0];
            t->m[right][1] += rgb[pix + 1];
            t->m[right][2] += rgb[pix + 2];
            t->R[right][0] += (double)rgb[pix + 0] * rgb[pix + 0];
            t->R[right][1] += (double)rgb[pix + 0] * rgb[pix + 1];
            t->R[right][2] += (double)rgb[pix + 0] * rgb[pix + 2];
            t->R[right][4] += (double)rgb[pix + 1] * rgb[pix + 1];
            t->R[right][5] += (double)rgb[pix + 1] * rgb[pix + 2];
            t->R[right][8] += (double)rgb[pix + 2] * rgb[pix + 2];
        }
    }

    /* fill symmetric entries */
    t->R[left][3]  = t->R[left][1];
    t->R[left][6]  = t->R[left][2];
    t->R[left][7]  = t->R[left][5];
    t->R[right][3] = t->R[right][1];
    t->R[right][6] = t->R[right][2];
    t->R[right][7] = t->R[right][5];

    t->leaf[k] = 0;
    t->child[k][0] = left;
    t->child[k][1] = right;

    if (t->cnt[left] > 0.5) {
        t->leaf[left] = 1;
        bscq_compute_node(t, left);
    }
    if (t->cnt[right] > 0.5) {
        t->leaf[right] = 1;
        bscq_compute_node(t, right);
    }

    t->num_nodes = right;
    return 1;
}

/* ======================================================================
 * BSCQ: find leaf with largest eigenvalue
 * ====================================================================== */

static int
bscq_find_max_eigen(BscqTree *t)
{
    int k, best = -1;
    double best_lambda = -1e100;

    for (k = 1; k <= t->num_nodes; k++) {
        if (!t->leaf[k]) continue;
        bscq_compute_eigen(t, k);
        if (t->lambda[k] > best_lambda) {
            best_lambda = t->lambda[k];
            best = k;
        }
    }
    return best;
}

/* ======================================================================
 * BSCQ: build the full palette tree
 * ====================================================================== */

static int
bscq_build(BscqTree *t, const unsigned char *rgb,
           int width, int height, uint16_t *cluster, int max_colors)
{
    int n, npixels = width * height;
    int m;

    if (max_colors < 2) max_colors = 2;
    if (max_colors > BSCQ_MAX_COLORS) max_colors = BSCQ_MAX_COLORS;

    t->num_nodes = 1;

    /* init root node (index 1) */
    memset(t->m[1], 0, 3 * sizeof(double));
    memset(t->R[1], 0, 9 * sizeof(double));
    t->cnt[1] = 0.0;
    t->leaf[1] = 1;
    t->child[1][0] = t->child[1][1] = -1;
    t->eigen_ok[1] = 0;

    for (n = 0; n < npixels; n++) {
        int pix = n * 3;
        cluster[n] = 1;
        t->cnt[1] += 1.0;
        t->m[1][0] += rgb[pix + 0];
        t->m[1][1] += rgb[pix + 1];
        t->m[1][2] += rgb[pix + 2];
        t->R[1][0] += (double)rgb[pix + 0] * rgb[pix + 0];
        t->R[1][1] += (double)rgb[pix + 0] * rgb[pix + 1];
        t->R[1][2] += (double)rgb[pix + 0] * rgb[pix + 2];
        t->R[1][4] += (double)rgb[pix + 1] * rgb[pix + 1];
        t->R[1][5] += (double)rgb[pix + 1] * rgb[pix + 2];
        t->R[1][8] += (double)rgb[pix + 2] * rgb[pix + 2];
    }
    t->R[1][3] = t->R[1][1];
    t->R[1][6] = t->R[1][2];
    t->R[1][7] = t->R[1][5];

    bscq_compute_node(t, 1);

    for (m = 1; m < max_colors; m++) {
        int k = bscq_find_max_eigen(t);
        if (k < 0) break;
        if (!bscq_split(t, rgb, width, height, cluster, k)) break;
    }

    return m;
}

/* ======================================================================
 * Map pixels to palette indices via tree traversal (undithered)
 * ====================================================================== */

static void
bscq_map_pixels(BscqTree *t, const unsigned char *rgb,
                int width, int height, const uint16_t *cluster,
                unsigned char *indices, int leaf_to_idx[], int *num_leaves)
{
    int n, npixels = width * height;
    int k, leaf_idx;

    *num_leaves = 0;
    for (k = 1; k <= t->num_nodes; k++) {
        if (t->leaf[k]) {
            leaf_to_idx[k] = (*num_leaves)++;
        } else {
            leaf_to_idx[k] = -1;
        }
    }

    for (n = 0; n < npixels; n++) {
        k = cluster[n];
        leaf_idx = leaf_to_idx[k];
        if (leaf_idx < 0) {
            /* fall back: exhaustive search */
            double best_d = 1e100;
            int best_k = 1;
            int pix = n * 3;
            for (k = 1; k <= t->num_nodes; k++) {
                double dr, dg, db, d;
                if (!t->leaf[k]) continue;
                dr = t->q[k][0] - rgb[pix + 0];
                dg = t->q[k][1] - rgb[pix + 1];
                db = t->q[k][2] - rgb[pix + 2];
                d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; best_k = k; }
            }
            leaf_idx = leaf_to_idx[best_k];
        }
        indices[n] = (unsigned char) leaf_idx;
    }
}

/* ======================================================================
 * Floyd-Steinberg error diffusion
 * ====================================================================== */

static void
floyd_steinberg_dither(const unsigned char *rgb,
                       int width, int height,
                       const unsigned char *palette, int npalette,
                       unsigned char *indices)
{
    int x, y, n, npixels, stride;
    double *err_buf;

    if (width <= 0 || height <= 0 || npalette < 2) return;

    npixels = width * height;
    stride = width + 2;  /* +2 for right guard column */

    err_buf = (double *)calloc((size_t)(height + 1) * stride * 3,
                                sizeof(double));
    if (!err_buf) {
        /* fall back: undithered nearest-neighbor */
        for (n = 0; n < npixels; n++) {
            int qi, best = 0;
            double best_d = 1e100;
            for (qi = 0; qi < npalette; qi++) {
                const unsigned char *p = palette + (size_t)qi * 3;
                double dr = (double)rgb[n * 3 + 0] - p[0];
                double dg = (double)rgb[n * 3 + 1] - p[1];
                double db = (double)rgb[n * 3 + 2] - p[2];
                double d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; best = qi; }
            }
            indices[n] = (unsigned char) best;
        }
        return;
    }

    for (n = 0; n < npixels; n++) {
        y = n / width;
        x = n % width;

        {
            double cur[3], best_d;
            int c, qi, q_idx;

            /* apply accumulated error */
            for (c = 0; c < 3; c++) {
                cur[c] = (double)rgb[n * 3 + c]
                       + err_buf[(y * stride + x) * 3 + c];
                if (cur[c] < 0.0) cur[c] = 0.0;
                if (cur[c] > 255.0) cur[c] = 255.0;
            }

            /* nearest palette entry */
            best_d = 1e100;
            q_idx = 0;
            for (qi = 0; qi < npalette; qi++) {
                const unsigned char *p = palette + (size_t)qi * 3;
                double dr = cur[0] - p[0];
                double dg = cur[1] - p[1];
                double db = cur[2] - p[2];
                double d = dr * dr + dg * dg + db * db;
                if (d < best_d) { best_d = d; q_idx = qi; }
            }

            indices[n] = (unsigned char) q_idx;

            /* distribute error via Floyd-Steinberg kernel */
            {
                double err_r = cur[0] - (double)palette[q_idx * 3 + 0];
                double err_g = cur[1] - (double)palette[q_idx * 3 + 1];
                double err_b = cur[2] - (double)palette[q_idx * 3 + 2];

                /* right: 7/16 */
                err_buf[(y * stride + x + 1) * 3 + 0] += err_r * (7.0 / 16.0);
                err_buf[(y * stride + x + 1) * 3 + 1] += err_g * (7.0 / 16.0);
                err_buf[(y * stride + x + 1) * 3 + 2] += err_b * (7.0 / 16.0);

                /* bottom-left: 3/16 */
                if (x > 0) {
                    err_buf[((y + 1) * stride + x - 1) * 3 + 0]
                        += err_r * (3.0 / 16.0);
                    err_buf[((y + 1) * stride + x - 1) * 3 + 1]
                        += err_g * (3.0 / 16.0);
                    err_buf[((y + 1) * stride + x - 1) * 3 + 2]
                        += err_b * (3.0 / 16.0);
                }

                /* bottom: 5/16 */
                err_buf[((y + 1) * stride + x) * 3 + 0] += err_r * (5.0 / 16.0);
                err_buf[((y + 1) * stride + x) * 3 + 1] += err_g * (5.0 / 16.0);
                err_buf[((y + 1) * stride + x) * 3 + 2] += err_b * (5.0 / 16.0);

                /* bottom-right: 1/16 */
                if (x + 1 < width) {
                    err_buf[((y + 1) * stride + x + 1) * 3 + 0]
                        += err_r * (1.0 / 16.0);
                    err_buf[((y + 1) * stride + x + 1) * 3 + 1]
                        += err_g * (1.0 / 16.0);
                    err_buf[((y + 1) * stride + x + 1) * 3 + 2]
                        += err_b * (1.0 / 16.0);
                }
            }
        }
    }

    free(err_buf);
}

/* ======================================================================
 * Public entry point
 * ====================================================================== */

int
bscqQuantize(const unsigned char *rgb, int width, int height,
             int maxColors, int doDither,
             unsigned char *colormapOut, unsigned char *indicesOut,
             int *outColors)
{
    int npixels = width * height;
    BscqTree *t;
    uint16_t *cluster;
    int *leaf_to_idx;
    int nleaf, k, i;

    if (width <= 0 || height <= 0 || !rgb || !colormapOut ||
        !indicesOut || !outColors)
        return 0;

    if (maxColors < 2) maxColors = 2;
    if (maxColors > BSCQ_MAX_COLORS) maxColors = BSCQ_MAX_COLORS;

    t = (BscqTree *)calloc(1, sizeof(BscqTree));
    cluster = (uint16_t *)malloc((size_t)npixels * sizeof(uint16_t));
    leaf_to_idx = (int *)malloc((BSCQ_MAX_NODES + 1) * sizeof(int));

    if (!t || !cluster || !leaf_to_idx) {
        free(t); free(cluster); free(leaf_to_idx);
        return 0;
    }

    /* build BSCQ tree and generate palette */
    bscq_build(t, rgb, width, height, cluster, maxColors);

    /* collect palette from leaf nodes */
    nleaf = 0;
    for (k = 1; k <= t->num_nodes; k++) {
        int val;
        if (!t->leaf[k]) continue;
        for (i = 0; i < 3; i++) {
            val = (int)(t->q[k][i] + 0.5);
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            colormapOut[nleaf * 3 + i] = (unsigned char) val;
        }
        nleaf++;
    }

    if (doDither && nleaf >= 2) {
        floyd_steinberg_dither(rgb, width, height,
                               colormapOut, nleaf, indicesOut);
    } else {
        bscq_map_pixels(t, rgb, width, height, cluster,
                        indicesOut, leaf_to_idx, &nleaf);
    }

    *outColors = nleaf;
    free(t);
    free(cluster);
    free(leaf_to_idx);
    return 1;
}
