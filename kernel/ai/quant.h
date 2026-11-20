/* GGML tensor formats: sizes, dequantization and quantized matrix multiply. */
#pragma once

#include "platform.h"

enum ggml_type {
    GGML_TYPE_F32 = 0,
    GGML_TYPE_F16 = 1,
    GGML_TYPE_Q4_0 = 2,
    GGML_TYPE_Q4_1 = 3,
    GGML_TYPE_Q5_0 = 6,
    GGML_TYPE_Q5_1 = 7,
    GGML_TYPE_Q8_0 = 8,
    GGML_TYPE_Q8_1 = 9,
    GGML_TYPE_Q2_K = 10,
    GGML_TYPE_Q3_K = 11,
    GGML_TYPE_Q4_K = 12,
    GGML_TYPE_Q5_K = 13,
    GGML_TYPE_Q6_K = 14,
    GGML_TYPE_Q8_K = 15,
    GGML_TYPE_BF16 = 30,
};

size_t ggml_blck_size(int type);  /* elements per block, 0 if unknown */
size_t ggml_type_size(int type);  /* bytes per block, 0 if unknown */
const char *ggml_type_name(int type);
bool ggml_type_supported(int type); /* can we compute with it? */

/* Row-major matrix of `rows` rows, each `cols` elements of `type`. */
struct qmat {
    const u8 *data;
    int type;
    int rows;
    int cols;
    size_t row_bytes;
    int chunk; /* dequantization chunk (elements), divides cols */
};

bool qmat_init(struct qmat *m, const void *data, int type, int cols, int rows);

/* Dequantize row r into out[cols]. */
void qmat_get_row(const struct qmat *m, int r, float *out);

/* out[t*out_stride + r] = dot(row r, x[t*x_stride ...]) for t in [0, T). */
void qmat_mul(const struct qmat *m, const float *x, int x_stride, float *out, int out_stride,
              int T);

/* Dequantize n elements (multiple of the block size) starting at src. */
void dequantize(int type, const void *src, float *dst, int n);

#define AI_MAX_BATCH 64

/* Matmul arithmetic: integer dot products (default; uses SDOT when the
 * CPU has it) or the float dequantize-then-dot path (reference). */
enum { QMODE_AUTO, QMODE_FLOAT, QMODE_BASE, QMODE_DOTPROD };
void qmat_set_mode(int mode);

/* Integer path (qdot.c). qdot_mul returns false if it can't handle m. */
bool qdot_type_supported(int type);
bool qdot_mul(const struct qmat *m, const float *x, int x_stride, float *out, int out_stride,
              int T, int mode);
bool qdot_unpack_row(const struct qmat *m, int r, float *out); /* test hook */

float vec_dot_f32(const float *a, const float *b, int n);
