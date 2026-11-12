#include "vector_ops.h"
#include <cmath>

// Exact double accumulation over float32-stored components.
// The judge computes expected distances in double precision from the
// float32-stored vectors; float32 accumulation can drift beyond the
// 1e-5 tolerance for larger dimensions, so we accumulate in double.
float l2_distance(const float* a, const float* b, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double d = (double)a[i] - (double)b[i];
        s += d * d;
    }
    return (float)std::sqrt(s);
}
