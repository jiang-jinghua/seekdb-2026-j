#include "vector_ops.h"
#include <cmath>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#define MVDB_X86 1
#include <immintrin.h>
#endif

static float l2_sq_scalar(const float* a, const float* b, size_t n) {
    float s = 0.f;
    for (size_t i = 0; i < n; ++i) { float d = a[i] - b[i]; s += d * d; }
    return s;
}

#if MVDB_X86
__attribute__((target("sse2")))
static float l2_sq_sse2(const float* a, const float* b, size_t n) {
    __m128 sum = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        __m128 d  = _mm_sub_ps(va, vb);
        sum = _mm_add_ps(sum, _mm_mul_ps(d, d));
    }
    float tmp[4];
    _mm_storeu_ps(tmp, sum);
    float s = tmp[0] + tmp[1] + tmp[2] + tmp[3];
    s += l2_sq_scalar(a + i, b + i, n - i);
    return s;
}

__attribute__((target("avx2")))
static float l2_sq_avx2(const float* a, const float* b, size_t n) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 d  = _mm256_sub_ps(va, vb);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(d, d));
    }
    float tmp[8];
    _mm256_storeu_ps(tmp, sum);
    float s = 0.f;
    for (int k = 0; k < 8; ++k) s += tmp[k];
    s += l2_sq_scalar(a + i, b + i, n - i);
    return s;
}
#endif

float l2_distance(const float* a, const float* b, size_t n) {
    float sq;
#if MVDB_X86
    static const bool has_avx2 = __builtin_cpu_supports("avx2");
    if (has_avx2) sq = l2_sq_avx2(a, b, n);
    else          sq = l2_sq_sse2(a, b, n);
#else
    sq = l2_sq_scalar(a, b, n);
#endif
    return std::sqrt(sq);
}
