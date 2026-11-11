#pragma once
#include <cstddef>
#include <cstdint>

// Squared-L2 is computed internally; public API returns true L2 (sqrt) for display.
// SIMD-dispatched: AVX2 if available, else SSE2, else scalar.
float l2_distance(const float* a, const float* b, size_t n);
