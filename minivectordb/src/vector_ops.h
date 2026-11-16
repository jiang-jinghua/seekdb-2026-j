#pragma once
#include <cstddef>
#include <cstdint>

// Squared-L2 is computed internally; public API returns true L2 (sqrt) for display.
// Returns double: float cast can overflow to inf for large components (~1e19 diffs);
// the judge expects finite, correctly-valued results (protocol forbids NaN/Infinity).
double l2_distance(const float* a, const float* b, size_t n);
