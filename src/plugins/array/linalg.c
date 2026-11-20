/*
 * array/linalg.c - linear algebra: the matrix product (the system's BLAS
 * when there is one: Accelerate on macOS, OpenBLAS on Linux and Windows;
 * else a blocked kernel of our own, on several threads), and the
 * decompositions (LU, Cholesky, QR, SVD, symmetric eigen) in float64.
 * (Part of array.c.)
 */

/* ======================================================================
 * The system's BLAS, opened when the first product needs it
 * ==================================================================== */

typedef void (*DgemmFn)(int order, int ta, int tb, int m, int n, int k, double alpha, const double *a, int lda,
                        const double *b, int ldb, double beta, double *c, int ldc);
typedef void (*SgemmFn)(int order, int ta, int tb, int m, int n, int k, float alpha, const float *a, int lda,
                        const float *b, int ldb, float beta, float *c, int ldc);
enum { CBLAS_ROW_MAJOR = 101, CBLAS_NO_TRANS = 111, CBLAS_TRANS = 112 };

static LubeLock *blas_lock;
static int blas_tried, blas_off;
static DgemmFn blas_dgemm;
static SgemmFn blas_sgemm;
static const char *blas_name = "none";

static void blas_open(void) {
    lube->lock_acquire(blas_lock);
    if (!blas_tried) {
        blas_tried = 1;
#if defined(__APPLE__)
        void *h = dlopen("/System/Library/Frameworks/Accelerate.framework/Accelerate", RTLD_LAZY | RTLD_LOCAL);
        if (h) {
            *(void **)&blas_dgemm = dlsym(h, "cblas_dgemm");
            *(void **)&blas_sgemm = dlsym(h, "cblas_sgemm");
            blas_name = "Accelerate";
        }
#elif defined(_WIN32)  /* LCOV_EXCL_START: (other systems': CI builds them) */
        static const char *const names[] = {"libopenblas.dll", "openblas.dll"};
        for (size_t i = 0; i < sizeof names / sizeof names[0] && !blas_dgemm; i++) {
            HMODULE h = LoadLibraryA(names[i]);
            if (!h) continue;
            FARPROC d = GetProcAddress(h, "cblas_dgemm"), s = GetProcAddress(h, "cblas_sgemm");
            memcpy(&blas_dgemm, &d, sizeof d);  /* (a function pointer of another type: copied, not cast) */
            memcpy(&blas_sgemm, &s, sizeof s);
            blas_name = "OpenBLAS";
        }
#else
        static const char *const names[] = {"libopenblas.so.0", "libopenblas.so", "libcblas.so.3", "libblas.so.3"};
        for (size_t i = 0; i < sizeof names / sizeof names[0] && !blas_dgemm; i++) {
            void *h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
            if (!h) continue;
            *(void **)&blas_dgemm = dlsym(h, "cblas_dgemm");
            *(void **)&blas_sgemm = dlsym(h, "cblas_sgemm");
            blas_name = i < 2 ? "OpenBLAS" : "BLAS";
        }
#endif  /* LCOV_EXCL_STOP */
        if (!blas_dgemm || !blas_sgemm) {  /* LCOV_EXCL_START: (Accelerate is always there) */
            blas_dgemm = NULL;
            blas_sgemm = NULL;
            blas_name = "none";
        }  /* LCOV_EXCL_STOP */
    }
    lube->lock_release(blas_lock);
}

/* ======================================================================
 * Our own product: C = A B, A packed in panels of MR rows, B of NR
 * columns, blocks sized for the caches; rows of blocks on several threads
 * ==================================================================== */

#define KC 256
#define MC 96
#define NC 4096

/* A matrix as gemm reads it: element (i, j) is at p[i * rs + j * cs]. */
#define AT(p, rs, cs, i, j) ((p)[(i) * (rs) + (j) * (cs)])

#define GEMM_PARTS(T, MR, NR, SUFFIX)                                                                          \
    static void kernel_##SUFFIX(int64_t kc, const T *a, const T *b, T *c, int64_t ldc, int64_t mr, int64_t nr, \
                                int add) {                                                                     \
        FUSED /* (here they are worth it: products need not match bit for bit across systems) */              \
        T acc[MR][NR];                                                                                         \
        for (int i = 0; i < MR; i++)                                                                           \
            for (int j = 0; j < NR; j++) acc[i][j] = 0;                                                        \
        for (int64_t p = 0; p < kc; p++) {                                                                     \
            const T *bp = b + p * NR;                                                                          \
            for (int i = 0; i < MR; i++) {                                                                     \
                T ai = a[p * MR + i];                                                                          \
                for (int j = 0; j < NR; j++) acc[i][j] += ai * bp[j];                                          \
            }                                                                                                  \
        }                                                                                                      \
        for (int64_t i = 0; i < mr; i++)                                                                       \
            for (int64_t j = 0; j < nr; j++) c[i * ldc + j] = (add ? c[i * ldc + j] : 0) + acc[i][j];        \
    }                                                                                                          \
    /* rows i0.. (mc of them) and columns p0.. (kc) of A, in panels of MR rows (padded with zeros) */          \
    static void pack_a_##SUFFIX(const T *a, int64_t rs, int64_t cs, int64_t i0, int64_t mc, int64_t p0,       \
                                int64_t kc, T *out) {                                                          \
        for (int64_t ir = 0; ir < mc; ir += MR)                                                                \
            for (int64_t p = 0; p < kc; p++)                                                                   \
                for (int64_t i = 0; i < MR; i++)                                                               \
                    *out++ = ir + i < mc ? AT(a, rs, cs, i0 + ir + i, p0 + p) : 0;                             \
    }                                                                                                          \
    static void pack_b_##SUFFIX(const T *b, int64_t rs, int64_t cs, int64_t p0, int64_t kc, int64_t j0,       \
                                int64_t nc, T *out) {                                                          \
        for (int64_t jr = 0; jr < nc; jr += NR)                                                                \
            for (int64_t p = 0; p < kc; p++)                                                                   \
                for (int64_t j = 0; j < NR; j++)                                                               \
                    *out++ = jr + j < nc ? AT(b, rs, cs, p0 + p, j0 + jr + j) : 0;                             \
    }                                                                                                          \
    typedef struct {                                                                                           \
        const T *a, *bp;                                                                                       \
        int64_t ars, acs, m, nc, kc, p0, j0, ldc;                                                              \
        T *c;                                                                                                  \
        int add;                                                                                               \
    } Job_##SUFFIX;                                                                                            \
    static void rows_##SUFFIX(void *ctx, int64_t from, int64_t to) { /* blocks of MC rows */                   \
        Job_##SUFFIX *j = (Job_##SUFFIX *)ctx;                                                                 \
        T *ap = (T *)malloc(sizeof(T) * (size_t)(MC + MR) * KC);                                               \
        if (!ap) abort(); /* LCOV_EXCL_LINE: out of memory */                                                  \
        for (int64_t blk = from; blk < to; blk++) {                                                            \
            int64_t i0 = blk * MC, mc = j->m - i0 < MC ? j->m - i0 : MC;                                       \
            pack_a_##SUFFIX(j->a, j->ars, j->acs, i0, mc, j->p0, j->kc, ap);                                   \
            for (int64_t jr = 0; jr < j->nc; jr += NR)                                                         \
                for (int64_t ir = 0; ir < mc; ir += MR)                                                        \
                    kernel_##SUFFIX(j->kc, ap + ir * j->kc, j->bp + jr * j->kc,                                \
                                    j->c + (i0 + ir) * j->ldc + j->j0 + jr, j->ldc,                            \
                                    mc - ir < MR ? mc - ir : MR, j->nc - jr < NR ? j->nc - jr : NR, j->add);   \
        }                                                                                                      \
        free(ap);                                                                                              \
    }                                                                                                          \
    /* C (m x n, rows ldc apart) = A (m x k) B (k x n), each read through its row and column strides */       \
    static void own_gemm_##SUFFIX(int64_t m, int64_t n, int64_t k, const T *a, int64_t ars, int64_t acs,      \
                                  const T *b, int64_t brs, int64_t bcs, T *c, int64_t ldc) {                   \
        if (k == 0) {                                                                                          \
            for (int64_t i = 0; i < m; i++)                                                                    \
                for (int64_t j = 0; j < n; j++) c[i * ldc + j] = 0;                                            \
            return;                                                                                            \
        }                                                                                                      \
        if (m * n * k <= 32 * 32 * 32) { /* small: no packing */                                               \
            for (int64_t i = 0; i < m; i++) {                                                                  \
                for (int64_t j = 0; j < n; j++) c[i * ldc + j] = 0;                                            \
                for (int64_t p = 0; p < k; p++) {                                                              \
                    T x = AT(a, ars, acs, i, p);                                                               \
                    for (int64_t j = 0; j < n; j++) c[i * ldc + j] += x * AT(b, brs, bcs, p, j);              \
                }                                                                                              \
            }                                                                                                  \
            return;                                                                                            \
        }                                                                                                      \
        T *bp = (T *)malloc(sizeof(T) * (size_t)KC * (size_t)(NC + NR));                                       \
        if (!bp) abort(); /* LCOV_EXCL_LINE: out of memory */                                                  \
        for (int64_t j0 = 0; j0 < n; j0 += NC) {                                                               \
            int64_t nc = n - j0 < NC ? n - j0 : NC;                                                            \
            for (int64_t p0 = 0; p0 < k; p0 += KC) {                                                           \
                int64_t kc = k - p0 < KC ? k - p0 : KC;                                                        \
                pack_b_##SUFFIX(b, brs, bcs, p0, kc, j0, nc, bp);                                              \
                Job_##SUFFIX job = {a, bp, ars, acs, m, nc, kc, p0, j0, ldc, c, p0 > 0};                       \
                int64_t blocks = (m + MC - 1) / MC;                                                            \
                par_for(blocks, blocks * MC * nc * kc > PARALLEL_WORK * 64 ? 1 : blocks, rows_##SUFFIX, &job); \
            }                                                                                                  \
        }                                                                                                      \
        free(bp);                                                                                              \
    }

GEMM_PARTS(double, 4, 8, d)
GEMM_PARTS(float, 4, 16, s)

/* C (m x n) = A (m x k) B (k x n), each read through row and column strides (in elements); the system's BLAS
 * when it can take them. */
static void gemm(int dtype, int64_t m, int64_t n, int64_t k, const void *a, int64_t ars, int64_t acs, const void *b,
                 int64_t brs, int64_t bcs, void *c, int64_t ldc) {
    if (!blas_tried) blas_open();
    int ok = blas_dgemm && !blas_off && m > 0 && n > 0 && k > 0 && m < INT_MAX && n < INT_MAX && k < INT_MAX;
    int ta = acs != 1, tb = bcs != 1;  /* (a transposed matrix: its columns lie in rows) */
    int64_t lda = ta ? acs : ars, ldb = tb ? bcs : brs;
    if (ok && (ta ? ars == 1 : acs == 1) && (tb ? brs == 1 : bcs == 1) && lda >= (ta ? m : k) && lda >= 1 &&
        ldb >= (tb ? k : n) && ldb >= 1 && lda < INT_MAX && ldb < INT_MAX) {
        if (dtype == ARR_F64)
            blas_dgemm(CBLAS_ROW_MAJOR, ta ? CBLAS_TRANS : CBLAS_NO_TRANS, tb ? CBLAS_TRANS : CBLAS_NO_TRANS, (int)m,
                       (int)n, (int)k, 1.0, (const double *)a, (int)lda, (const double *)b, (int)ldb, 0.0, (double *)c,
                       (int)ldc);
        else
            blas_sgemm(CBLAS_ROW_MAJOR, ta ? CBLAS_TRANS : CBLAS_NO_TRANS, tb ? CBLAS_TRANS : CBLAS_NO_TRANS, (int)m,
                       (int)n, (int)k, 1.0f, (const float *)a, (int)lda, (const float *)b, (int)ldb, 0.0f, (float *)c,
                       (int)ldc);
        return;
    }
    if (dtype == ARR_F64) own_gemm_d(m, n, k, (const double *)a, ars, acs, (const double *)b, brs, bcs, (double *)c, ldc);
    else own_gemm_s(m, n, k, (const float *)a, ars, acs, (const float *)b, brs, bcs, (float *)c, ldc);
}

/* ======================================================================
 * matmul: a @ b
 * ==================================================================== */

/* x @ y, as numpy's matmul: 1-dimensional operands count as a row (left) or a column (right), and axes
 * before the last two are a stack of matrices, broadcast. */
static LubeValue matmul(LubeVM *vm, LubeValue xv, LubeValue yv) {
    Operand ox, oy;
    LubeValue err;
    if (!operand_of(vm, xv, &ox, &err)) return err;
    if (!operand_of(vm, yv, &oy, &err)) {
        operand_done(&ox);
        return err;
    }
    LubeValue r;
    if (!ox.a || !oy.a || !ox.a->ndim || !oy.a->ndim) {
        r = lube->error(vm, "matmul needs Arrays of at least 1 dimension (for a Number, use *)");
        goto done;
    }
    int t = promote(ox.a->dtype, oy.a->dtype), kind = t == ARR_F32 ? ARR_F32 : ARR_F64;
    Array *a, *b;
    LubeValue ka = arr_from(vm, xv, kind, &a), kb = arr_from(vm, yv, kind, &b);
    if (ox.made) {  /* (a list's: made again in the right dtype) */
        lube->release(ka);
        ka = arr_copy(vm, ox.a, kind, &a);
    }
    if (oy.made) {
        lube->release(kb);
        kb = arr_copy(vm, oy.a, kind, &b);
    }
    /* as matrices: (..., m, k) and (..., k, n) */
    Array A = *a, B = *b;
    if (A.ndim == 1) {
        A.ndim = 2;
        A.shape[1] = A.shape[0];
        A.strides[1] = A.strides[0];
        A.shape[0] = 1;
        A.strides[0] = 0;
    }
    if (B.ndim == 1) {
        B.ndim = 2;
        B.shape[1] = 1;
        B.strides[1] = 0;
    }
    int64_t m = A.shape[A.ndim - 2], k = A.shape[A.ndim - 1], k2 = B.shape[B.ndim - 2], n = B.shape[B.ndim - 1];
    if (k != k2) {
        char x[200], y[200];
        shape_text(x, sizeof x, a->ndim, a->shape);
        shape_text(y, sizeof y, b->ndim, b->shape);
        r = lube->error(vm, "matmul: shapes %s and %s do not fit (%.0f is not %.0f)", x, y, (double)k, (double)k2);
        goto out;
    }
    /* each matrix's elements lie along rows or along columns; else, a copy whose do */
    LubeValue ca = lube->retain(lube->nothing()), cb = lube->retain(lube->nothing());
    int64_t ars = A.strides[A.ndim - 2], acs = A.strides[A.ndim - 1], brs = B.strides[B.ndim - 2], bcs = B.strides[B.ndim - 1];
    if (!(acs == 1 || ars == 1 || m == 1 || k == 1)) {
        Array *c;
        lube->release(ca);
        ca = arr_copy(vm, &A, -1, &c);
        A = *c;
        ars = A.strides[A.ndim - 2];
        acs = 1;
    }
    if (!(bcs == 1 || brs == 1 || n == 1 || k == 1)) {
        Array *c;
        lube->release(cb);
        cb = arr_copy(vm, &B, -1, &c);
        B = *c;
        brs = B.strides[B.ndim - 2];
        bcs = 1;
    }
    if (m == 1) ars = k;  /* (one row: any row stride will do; keep BLAS happy) */
    if (k == 1) {
        acs = 1;
        brs = n > 1 && bcs == 1 ? n : brs;
    }
    if (n == 1) bcs = 1;
    /* the stack: the axes before the last two, broadcast */
    Array sa = A, sb = B;
    sa.ndim -= 2;
    sb.ndim -= 2;
    int nb;
    int64_t bshape[ARR_MAXDIM];
    if (!broadcast(&sa, &sb, &nb, bshape)) {
        r = broadcast_error(vm, a, b);
        lube->release(ca);
        lube->release(cb);
        goto out;
    }
    int64_t st_a[ARR_MAXDIM], st_b[ARR_MAXDIM], shape[ARR_MAXDIM];
    broadcast_strides(&sa, nb, bshape, st_a);
    broadcast_strides(&sb, nb, bshape, st_b);
    int nd = 0;
    for (int d = 0; d < nb; d++) shape[nd++] = bshape[d];
    if (a->ndim > 1) shape[nd++] = m;
    if (b->ndim > 1) shape[nd++] = n;
    Array *dst;
    r = arr_new(vm, kind, nd, shape, &dst, 0);
    if (dst) {
        int64_t batches = shape_size(nb, bshape), z = ITEMSIZE[kind];
        for (int64_t bi = 0; bi < batches; bi++) {
            int64_t oa = 0, ob = 0, rr = bi;
            for (int d = nb - 1; d >= 0; d--) {
                int64_t i = rr % bshape[d];
                rr /= bshape[d];
                oa += i * st_a[d];
                ob += i * st_b[d];
            }
            gemm(kind, m, n, k, A.data + oa * z, ars, acs, B.data + ob * z, brs, bcs, dst->data + bi * m * n * z, n);
        }
        if (!IS_FLOAT(t)) {  /* integers in, integers out (computed in float64: exact to 2^53) */
            Array *conv;
            LubeValue cv = arr_copy(vm, dst, t == ARR_BOOL ? ARR_I64 : t, &conv);
            lube->release(r);
            r = cv;
        }
        if (nd == 0) {  /* vector @ vector: a Number */
            LubeValue num = elem_value(arr_of(r)->data, arr_of(r)->dtype);
            lube->release(r);
            r = num;
        }
    }
    lube->release(ca);
    lube->release(cb);
out:
    lube->release(ka);
    lube->release(kb);
done:
    operand_done(&ox);
    operand_done(&oy);
    return r;
}

/* ======================================================================
 * Decompositions, in float64 on a copy of their own
 * ==================================================================== */

/* A square or any matrix, as float64 and C-contiguous: 0 with an error unless it is a matrix (square if asked). */
static int matrix_arg(LubeVM *vm, const char *fname, LubeValue v, int square, LubeValue *keep, Array **out,
                      LubeValue *err) {
    Array *a;
    LubeValue made = arr_from(vm, v, -1, &a);
    if (!a) {
        *err = made;
        return 0;
    }
    if (a->ndim != 2 || (square && a->shape[0] != a->shape[1])) {
        lube->release(made);
        *err = lube->error(vm, "%s() needs a %smatrix (an Array of 2 dimensions)", fname, square ? "square " : "");
        return 0;
    }
    *keep = arr_copy(vm, a, ARR_F64, out);
    lube->release(made);
    return 1;
}

/* LU with partial pivoting, in place (n x n): 0 if the matrix is singular. */
static int lu(double *a, int64_t n, int64_t *piv, int *sign) {
    *sign = 1;
    int ok = 1;
    for (int64_t k = 0; k < n; k++) {
        int64_t p = k;
        for (int64_t i = k + 1; i < n; i++)
            if (fabs(a[i * n + k]) > fabs(a[p * n + k])) p = i;
        piv[k] = p;
        if (p != k) {
            *sign = -*sign;
            for (int64_t j = 0; j < n; j++) {
                double t = a[k * n + j];
                a[k * n + j] = a[p * n + j];
                a[p * n + j] = t;
            }
        }
        if (a[k * n + k] == 0) {
            ok = 0;
            continue;
        }
        for (int64_t i = k + 1; i < n; i++) {
            double f = a[i * n + k] /= a[k * n + k];
            for (int64_t j = k + 1; j < n; j++) a[i * n + j] -= f * a[k * n + j];
        }
    }
    return ok;
}

/* Solves LU x = b for nb right-hand sides (b: n x nb, rows nb apart), in place. */
static void lu_solve(const double *a, int64_t n, const int64_t *piv, double *b, int64_t nb) {
    for (int64_t k = 0; k < n; k++)
        if (piv[k] != k)
            for (int64_t j = 0; j < nb; j++) {
                double t = b[k * nb + j];
                b[k * nb + j] = b[piv[k] * nb + j];
                b[piv[k] * nb + j] = t;
            }
    for (int64_t i = 0; i < n; i++)
        for (int64_t k = 0; k < i; k++)
            for (int64_t j = 0; j < nb; j++) b[i * nb + j] -= a[i * n + k] * b[k * nb + j];
    for (int64_t i = n - 1; i >= 0; i--) {
        for (int64_t k = i + 1; k < n; k++)
            for (int64_t j = 0; j < nb; j++) b[i * nb + j] -= a[i * n + k] * b[k * nb + j];
        for (int64_t j = 0; j < nb; j++) b[i * nb + j] /= a[i * n + i];
    }
}

/* A result back in float32 when its input was float32. */
static LubeValue in_dtype(LubeVM *vm, LubeValue v, int f32) {
    if (!f32) return v;
    Array *c;
    LubeValue r = arr_copy(vm, arr_of(v), ARR_F32, &c);
    lube->release(v);
    return r;
}

static int was_f32(LubeValue v) {
    Array *a = arr_of(v);
    return a && a->dtype == ARR_F32;
}

/* solve(a, b): x with a x = b; inv(a) when b is NULL. */
static LubeValue solve_or_inv(LubeVM *vm, const char *fname, LubeValue av, const LubeValue *bv) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, fname, av, 1, &ka, &a, &err)) return err;
    int64_t n = a->shape[0];
    Array *x;
    LubeValue r;
    if (bv) {
        Array *b;
        LubeValue made = arr_from(vm, *bv, -1, &b);
        if (!b) {
            lube->release(ka);
            return made;
        }
        if ((b->ndim != 1 && b->ndim != 2) || b->shape[0] != n) {
            lube->release(made);
            lube->release(ka);
            return lube->error(vm, "solve(): b must have %.0f rows, as a has", (double)n);
        }
        r = arr_copy(vm, b, ARR_F64, &x);
        lube->release(made);
    } else {
        r = arr_new(vm, ARR_F64, 2, a->shape, &x, 1);
        for (int64_t i = 0; i < n; i++) ((double *)x->data)[i * n + i] = 1;
    }
    int64_t *piv = (int64_t *)malloc(sizeof(int64_t) * (size_t)(n ? n : 1));
    if (!piv) abort();  /* LCOV_EXCL_LINE: out of memory */
    int sign;
    if (!lu((double *)a->data, n, piv, &sign)) {
        lube->release(r);
        r = lube->error(vm, "%s(): the matrix is singular", fname);
    } else {
        lu_solve((const double *)a->data, n, piv, (double *)x->data, x->ndim == 2 ? x->shape[1] : 1);
        r = in_dtype(vm, r, was_f32(av) || (bv && was_f32(*bv)));
    }
    free(piv);
    lube->release(ka);
    return r;
}

/* det(a), or slogdet(a) ([sign, log of |det|]) */
static LubeValue determinant(LubeVM *vm, const char *fname, LubeValue av, int logs) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, fname, av, 1, &ka, &a, &err)) return err;
    int64_t n = a->shape[0];
    int64_t *piv = (int64_t *)malloc(sizeof(int64_t) * (size_t)(n ? n : 1));
    if (!piv) abort();  /* LCOV_EXCL_LINE: out of memory */
    int sign;
    double *d = (double *)a->data, det = 1, logdet = 0;
    if (!lu(d, n, piv, &sign)) {
        det = 0;
        sign = 0;
        logdet = -INFINITY;
    } else {
        det = sign;
        for (int64_t i = 0; i < n; i++) {
            det *= d[i * n + i];
            logdet += log(fabs(d[i * n + i]));
            if (d[i * n + i] < 0) sign = -sign;
        }
    }
    free(piv);
    lube->release(ka);
    if (!logs) return lube->number_value(det);
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, lube->number_value(sign));
    lube->list_push(r, lube->number_value(logdet));
    return r;
}

/* cholesky(a): L (lower) with L L^T = a */
static LubeValue cholesky(LubeVM *vm, LubeValue av) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, "cholesky", av, 1, &ka, &a, &err)) return err;
    int64_t n = a->shape[0];
    Array *l;
    LubeValue r = arr_new(vm, ARR_F64, 2, a->shape, &l, 1);
    const double *A = (const double *)a->data;
    double *L = (double *)l->data;
    for (int64_t j = 0; j < n; j++) {
        double s = A[j * n + j];
        for (int64_t k = 0; k < j; k++) s -= L[j * n + k] * L[j * n + k];
        if (!(s > 0)) {
            lube->release(r);
            lube->release(ka);
            return lube->error(vm, "cholesky(): the matrix is not positive definite");
        }
        L[j * n + j] = sqrt(s);
        for (int64_t i = j + 1; i < n; i++) {
            double t = A[i * n + j];
            for (int64_t k = 0; k < j; k++) t -= L[i * n + k] * L[j * n + k];
            L[i * n + j] = t / L[j * n + j];
        }
    }
    lube->release(ka);
    return in_dtype(vm, r, was_f32(av));
}

/* Householder QR of a (m x n, in place): R in its upper triangle, the reflectors below it and in tau. */
static void householder(double *a, int64_t m, int64_t n, double *tau) {
    int64_t kmax = m < n ? m : n;
    for (int64_t k = 0; k < kmax; k++) {
        double norm = 0;
        for (int64_t i = k; i < m; i++) norm += a[i * n + k] * a[i * n + k];
        norm = sqrt(norm);
        if (norm == 0) {
            tau[k] = 0;
            continue;
        }
        double alpha = a[k * n + k], beta = alpha > 0 ? -norm : norm;  /* (as LAPACK: R's diagonal -sign(alpha)) */
        tau[k] = (beta - alpha) / beta;
        double scale = 1 / (alpha - beta);
        for (int64_t i = k + 1; i < m; i++) a[i * n + k] *= scale;
        a[k * n + k] = beta;
        for (int64_t j = k + 1; j < n; j++) {  /* apply (I - tau v v^T) to the columns right of it */
            double s = a[k * n + j];
            for (int64_t i = k + 1; i < m; i++) s += a[i * n + k] * a[i * n + j];
            s *= tau[k];
            a[k * n + j] -= s;
            for (int64_t i = k + 1; i < m; i++) a[i * n + j] -= s * a[i * n + k];
        }
    }
}

/* Q's first `cols` columns (m x cols) from the reflectors householder() left in a (m x n). */
static void householder_q(const double *a, int64_t m, int64_t n, const double *tau, double *q, int64_t cols) {
    int64_t kmax = m < n ? m : n;
    for (int64_t i = 0; i < m; i++)
        for (int64_t j = 0; j < cols; j++) q[i * cols + j] = i == j;
    for (int64_t k = kmax - 1; k >= 0; k--) {
        if (tau[k] == 0) continue;
        for (int64_t j = 0; j < cols; j++) {
            double s = q[k * cols + j];
            for (int64_t i = k + 1; i < m; i++) s += a[i * n + k] * q[i * cols + j];
            s *= tau[k];
            q[k * cols + j] -= s;
            for (int64_t i = k + 1; i < m; i++) q[i * cols + j] -= s * a[i * n + k];
        }
    }
}

/* qr(a): [Q, R], reduced (Q: m x k, R: k x n, k the smaller of m and n), or complete (Q: m x m, R: m x n) */
static LubeValue qr(LubeVM *vm, LubeValue av, int complete) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, "qr", av, 0, &ka, &a, &err)) return err;
    int64_t m = a->shape[0], n = a->shape[1], k = m < n ? m : n, qc = complete ? m : k;
    double *tau = (double *)malloc(sizeof(double) * (size_t)(k ? k : 1));
    if (!tau) abort();  /* LCOV_EXCL_LINE: out of memory */
    double *A = (double *)a->data;
    householder(A, m, n, tau);
    int64_t qs[2] = {m, qc}, rs[2] = {qc, n};
    Array *q, *rr;
    LubeValue vq = arr_new(vm, ARR_F64, 2, qs, &q, 1), vr = arr_new(vm, ARR_F64, 2, rs, &rr, 1);
    householder_q(A, m, n, tau, (double *)q->data, qc);
    for (int64_t i = 0; i < qc; i++)
        for (int64_t j = i; j < n; j++) ((double *)rr->data)[i * n + j] = i < m ? A[i * n + j] : 0;
    free(tau);
    lube->release(ka);
    int f32 = was_f32(av);
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, in_dtype(vm, vq, f32));
    lube->list_push(r, in_dtype(vm, vr, f32));
    return r;
}

/* One-sided Jacobi SVD of a (m x n, m >= n, in place): a becomes U's columns times the singular values;
 * v (n x n) the right singular vectors. */
static void jacobi_svd(double *a, int64_t m, int64_t n, double *v) {
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) v[i * n + j] = i == j;
    for (int sweep = 0; sweep < 60; sweep++) {
        int rotated = 0;
        for (int64_t p = 0; p < n - 1; p++)
            for (int64_t q = p + 1; q < n; q++) {
                double alpha = 0, beta = 0, gamma = 0;
                for (int64_t i = 0; i < m; i++) {
                    double x = a[i * n + p], y = a[i * n + q];
                    alpha += x * x;
                    beta += y * y;
                    gamma += x * y;
                }
                if (fabs(gamma) <= 1e-15 * sqrt(alpha * beta) || gamma == 0) continue;
                rotated = 1;
                double zeta = (beta - alpha) / (2 * gamma);
                double t = (zeta >= 0 ? 1 : -1) / (fabs(zeta) + sqrt(1 + zeta * zeta));
                double c = 1 / sqrt(1 + t * t), s = c * t;
                for (int64_t i = 0; i < m; i++) {
                    double x = a[i * n + p], y = a[i * n + q];
                    a[i * n + p] = c * x - s * y;
                    a[i * n + q] = s * x + c * y;
                }
                for (int64_t i = 0; i < n; i++) {
                    double x = v[i * n + p], y = v[i * n + q];
                    v[i * n + p] = c * x - s * y;
                    v[i * n + q] = s * x + c * y;
                }
            }
        if (!rotated) break;
    }
}

/* Makes columns from..cols of q (m x cols, the first `from` orthonormal) orthonormal too, from the standard
 * basis: Gram-Schmidt, twice for accuracy. */
static void complete_basis(double *q, int64_t m, int64_t cols, int64_t from) {
    int64_t e = 0;
    for (int64_t c = from; c < cols; c++) {
        for (;; e++) {
            for (int64_t i = 0; i < m; i++) q[i * cols + c] = i == e % m;
            for (int pass = 0; pass < 2; pass++)
                for (int64_t k = 0; k < c; k++) {
                    double d = 0;
                    for (int64_t i = 0; i < m; i++) d += q[i * cols + k] * q[i * cols + c];
                    for (int64_t i = 0; i < m; i++) q[i * cols + c] -= d * q[i * cols + k];
                }
            double norm = 0;
            for (int64_t i = 0; i < m; i++) norm += q[i * cols + c] * q[i * cols + c];
            norm = sqrt(norm);
            if (norm > 1e-6) {
                for (int64_t i = 0; i < m; i++) q[i * cols + c] /= norm;
                e++;
                break;
            }
        }
    }
}

/* The SVD of a matrix (m x n) in float64: u (m x ucols), s (k, descending), vt (k x n), k = min(m, n);
 * ucols is m when full, else k. */
typedef struct {
    int64_t m, n, k, ucols;
    double *u, *s, *vt;
} Svd;

static void svd_free(Svd *d) {
    free(d->u);
    free(d->s);
    free(d->vt);
}

static void svd_of(const double *a, int64_t m, int64_t n, int full, Svd *out) {
    int tall = m >= n;
    int64_t rows = tall ? m : n, cols = tall ? n : m, k = cols;  /* (work on a or its transpose: rows >= cols) */
    double *w = (double *)malloc(sizeof(double) * (size_t)(rows * cols + 1));
    double *v = (double *)malloc(sizeof(double) * (size_t)(cols * cols + 1));
    if (!w || !v) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t i = 0; i < rows; i++)
        for (int64_t j = 0; j < cols; j++) w[i * cols + j] = tall ? a[i * n + j] : a[j * n + i];
    jacobi_svd(w, rows, cols, v);
    double *s = (double *)malloc(sizeof(double) * (size_t)(k + 1));
    int64_t *order = (int64_t *)malloc(sizeof(int64_t) * (size_t)(k + 1));
    if (!s || !order) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t j = 0; j < k; j++) {
        double norm = 0;
        for (int64_t i = 0; i < rows; i++) norm += w[i * cols + j] * w[i * cols + j];
        s[j] = sqrt(norm);
        order[j] = j;
    }
    for (int64_t i = 1; i < k; i++)  /* largest first (insertion: k is small next to the work above) */
        for (int64_t j = i; j > 0 && s[order[j]] > s[order[j - 1]]; j--) {
            int64_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    /* left vectors (rows x lc) from w's columns, right ones (k x cols) from v's */
    int64_t lc = tall ? (full ? m : k) : k;
    double *left = (double *)calloc((size_t)(rows * lc + 1), sizeof(double));
    double *right = (double *)malloc(sizeof(double) * (size_t)(k * cols + 1));
    double *sv = (double *)malloc(sizeof(double) * (size_t)(k + 1));
    if (!left || !right || !sv) abort();  /* LCOV_EXCL_LINE: out of memory */
    double smax = k ? s[order[0]] : 0;
    int64_t good = 0;
    for (int64_t j = 0; j < k; j++) {
        int64_t o = order[j];
        sv[j] = s[o];
        for (int64_t i = 0; i < cols; i++) right[j * cols + i] = v[i * cols + o];
        if (s[o] > smax * 1e-15 && s[o] > 0) {
            for (int64_t i = 0; i < rows; i++) left[i * lc + j] = w[i * cols + o] / s[o];
            good = j + 1;
        }
    }
    complete_basis(left, rows, lc, good);  /* (for zero singular values, and the full U's extra columns) */
    free(w);
    free(v);
    free(s);
    free(order);
    out->m = m;
    out->n = n;
    out->k = k;
    out->s = sv;
    if (tall) {  /* a = left diag(s) right */
        out->u = left;
        out->ucols = lc;
        out->vt = right;
    } else {  /* a^T = left diag(s) right, so a = right^T diag(s) left^T */
        out->ucols = full ? m : k;
        out->u = (double *)calloc((size_t)(m * out->ucols + 1), sizeof(double));
        out->vt = (double *)calloc((size_t)(k * n + 1), sizeof(double));
        if (!out->u || !out->vt) abort();  /* LCOV_EXCL_LINE: out of memory */
        for (int64_t i = 0; i < m; i++)
            for (int64_t j = 0; j < k; j++) out->u[i * out->ucols + j] = right[j * cols + i];
        complete_basis(out->u, m, out->ucols, k);
        for (int64_t j = 0; j < k; j++)
            for (int64_t i = 0; i < n; i++) out->vt[j * n + i] = left[i * lc + j];
        free(left);
        free(right);
    }
}

static LubeValue f64_array(LubeVM *vm, int nd, const int64_t *shape, const double *data) {
    Array *a;
    LubeValue v = arr_new(vm, ARR_F64, nd, shape, &a, 0);
    if (a->size) memcpy(a->data, data, sizeof(double) * (size_t)a->size);
    return v;
}

/* svd(a, full): [U, S, Vh] */
static LubeValue svd(LubeVM *vm, LubeValue av, int full, int values_only) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, "svd", av, 0, &ka, &a, &err)) return err;
    Svd d;
    svd_of((const double *)a->data, a->shape[0], a->shape[1], full, &d);
    lube->release(ka);
    int f32 = was_f32(av);
    int64_t us[2] = {d.m, d.ucols}, vs[2] = {full ? d.n : d.k, d.n};
    LubeValue s = in_dtype(vm, f64_array(vm, 1, &d.k, d.s), f32);
    if (values_only) {
        svd_free(&d);
        return s;
    }
    double *vt = d.vt;
    if (full && d.n > d.k) {  /* Vh complete: n x n */
        vt = (double *)calloc((size_t)(d.n * d.n), sizeof(double));
        if (!vt) abort();  /* LCOV_EXCL_LINE: out of memory */
        double *col = (double *)calloc((size_t)(d.n * d.n), sizeof(double));  /* (V, columns) */
        if (!col) abort();  /* LCOV_EXCL_LINE: out of memory */
        for (int64_t j = 0; j < d.k; j++)
            for (int64_t i = 0; i < d.n; i++) col[i * d.n + j] = d.vt[j * d.n + i];
        complete_basis(col, d.n, d.n, d.k);
        for (int64_t j = 0; j < d.n; j++)
            for (int64_t i = 0; i < d.n; i++) vt[j * d.n + i] = col[i * d.n + j];
        free(col);
    }
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, in_dtype(vm, f64_array(vm, 2, us, d.u), f32));
    lube->list_push(r, s);
    lube->list_push(r, in_dtype(vm, f64_array(vm, 2, vs, vt), f32));
    if (vt != d.vt) free(vt);
    svd_free(&d);
    return r;
}

/* eigh(a): [w, v], the eigenvalues of a symmetric matrix (ascending) and its eigenvectors (columns), by
 * Jacobi rotations; only the lower triangle is read, as numpy's does. */
static LubeValue eigh(LubeVM *vm, LubeValue av, int values_only) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, values_only ? "eigvalsh" : "eigh", av, 1, &ka, &a, &err)) return err;
    int64_t n = a->shape[0];
    double *A = (double *)a->data;
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = i + 1; j < n; j++) A[i * n + j] = A[j * n + i];
    double *V = (double *)malloc(sizeof(double) * (size_t)(n * n + 1));
    if (!V) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) V[i * n + j] = i == j;
    for (int sweep = 0; sweep < 100; sweep++) {
        double off = 0;
        for (int64_t i = 0; i < n; i++)
            for (int64_t j = i + 1; j < n; j++) off += A[i * n + j] * A[i * n + j];
        if (off < 1e-300) break;
        for (int64_t p = 0; p < n - 1; p++)
            for (int64_t q = p + 1; q < n; q++) {
                double apq = A[p * n + q];
                if (fabs(apq) < 1e-300) continue;
                double theta = (A[q * n + q] - A[p * n + p]) / (2 * apq);
                double t = (theta >= 0 ? 1 : -1) / (fabs(theta) + sqrt(theta * theta + 1));
                double c = 1 / sqrt(t * t + 1), s = t * c, tau = s / (1 + c);
                for (int64_t k = 0; k < n; k++) {  /* (the classic update: the pair's own entries set directly) */
                    if (k == p || k == q) continue;
                    double akp = A[k * n + p], akq = A[k * n + q];
                    A[k * n + p] = A[p * n + k] = akp - s * (akq + tau * akp);
                    A[k * n + q] = A[q * n + k] = akq + s * (akp - tau * akq);
                }
                A[p * n + p] -= t * apq;
                A[q * n + q] += t * apq;
                A[p * n + q] = A[q * n + p] = 0;
                for (int64_t k = 0; k < n; k++) {
                    double x = V[k * n + p], y = V[k * n + q];
                    V[k * n + p] = x - s * (y + tau * x);
                    V[k * n + q] = y + s * (x - tau * y);
                }
            }
    }
    int64_t *order = (int64_t *)malloc(sizeof(int64_t) * (size_t)(n + 1));
    if (!order) abort();  /* LCOV_EXCL_LINE: out of memory */
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t i = 1; i < n; i++)
        for (int64_t j = i; j > 0 && A[order[j] * n + order[j]] < A[order[j - 1] * n + order[j - 1]]; j--) {
            int64_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    Array *w, *vec;
    int64_t sq[2] = {n, n};
    LubeValue vw = arr_new(vm, ARR_F64, 1, &n, &w, 0), vv = arr_new(vm, ARR_F64, 2, sq, &vec, 0);
    for (int64_t j = 0; j < n; j++) {
        ((double *)w->data)[j] = A[order[j] * n + order[j]];
        for (int64_t i = 0; i < n; i++) ((double *)vec->data)[i * n + j] = V[i * n + order[j]];
    }
    free(V);
    free(order);
    lube->release(ka);
    int f32 = was_f32(av);
    if (values_only) {
        lube->release(vv);
        return in_dtype(vm, vw, f32);
    }
    LubeValue r = lube->new_list(vm);
    lube->list_push(r, in_dtype(vm, vw, f32));
    lube->list_push(r, in_dtype(vm, vv, f32));
    return r;
}

/* pinv(a) and lstsq(a, b) ([x, residuals, rank, s]) through the SVD: singular values at most cutoff count
 * as zero. */
static LubeValue pinv_or_lstsq(LubeVM *vm, LubeValue av, const LubeValue *bv, double rcond) {
    LubeValue ka, err;
    Array *a;
    if (!matrix_arg(vm, bv ? "lstsq" : "pinv", av, 0, &ka, &a, &err)) return err;
    int64_t m = a->shape[0], n = a->shape[1];
    Array *b = NULL;
    LubeValue kb = lube->retain(lube->nothing());
    if (bv) {
        Array *given;
        LubeValue made = arr_from(vm, *bv, -1, &given);
        if (!given) {
            lube->release(ka);
            lube->release(kb);
            return made;
        }
        if ((given->ndim != 1 && given->ndim != 2) || given->shape[0] != m) {
            lube->release(made);
            lube->release(ka);
            lube->release(kb);
            return lube->error(vm, "lstsq(): b must have %.0f rows, as a has", (double)m);
        }
        lube->release(kb);
        kb = arr_copy(vm, given, ARR_F64, &b);
        lube->release(made);
    }
    Svd d;
    svd_of((const double *)a->data, m, n, 0, &d);
    double cutoff = (rcond < 0 ? (double)(m > n ? m : n) * 2.220446049250313e-16 : rcond) * (d.k ? d.s[0] : 0);
    int64_t rank = 0;
    for (int64_t j = 0; j < d.k; j++) rank += d.s[j] > cutoff;
    /* pinv = V diag(1/s) U^T: n x m */
    int64_t ps[2] = {n, m};
    Array *p;
    LubeValue vp = arr_new(vm, ARR_F64, 2, ps, &p, 1);
    double *P = (double *)p->data;
    for (int64_t j = 0; j < rank; j++)
        for (int64_t i = 0; i < n; i++) {
            double f = d.vt[j * n + i] / d.s[j];
            for (int64_t r = 0; r < m; r++) P[i * m + r] += f * d.u[r * d.ucols + j];
        }
    int f32 = was_f32(av);
    LubeValue result;
    if (!bv) {
        result = in_dtype(vm, vp, f32);
    } else {
        int64_t nb = b->ndim == 2 ? b->shape[1] : 1;
        int64_t xs[2] = {n, nb};
        Array *x;
        LubeValue vx = arr_new(vm, ARR_F64, b->ndim, xs, &x, 1);
        const double *B = (const double *)b->data;
        double *X = (double *)x->data;
        for (int64_t i = 0; i < n; i++)
            for (int64_t r = 0; r < m; r++)
                for (int64_t j = 0; j < nb; j++) X[i * nb + j] += P[i * m + r] * B[r * nb + j];
        int64_t nres = rank == n && m > n ? nb : 0;  /* (numpy gives residuals only then) */
        Array *res;
        LubeValue vres = arr_new(vm, ARR_F64, 1, &nres, &res, 1);
        for (int64_t j = 0; j < nres; j++) {
            double ss = 0;
            for (int64_t r = 0; r < m; r++) {
                double e = B[r * nb + j];
                for (int64_t i = 0; i < n; i++) e -= ((const double *)a->data)[r * n + i] * X[i * nb + j];
                ss += e * e;
            }
            ((double *)res->data)[j] = ss;
        }
        result = lube->new_list(vm);
        lube->list_push(result, in_dtype(vm, vx, f32));
        lube->list_push(result, in_dtype(vm, vres, f32));
        lube->list_push(result, lube->number_value((double)rank));
        lube->list_push(result, in_dtype(vm, f64_array(vm, 1, &d.k, d.s), f32));
        lube->release(vp);
    }
    svd_free(&d);
    lube->release(ka);
    lube->release(kb);
    return result;
}
