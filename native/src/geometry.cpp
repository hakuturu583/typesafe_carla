// Issue #23: the transform matrices behind Transform.get_matrix & co.
//
// LibCarla computes them, not the Codon layer, because its rotation sign
// convention for pitch and roll changed between CARLA 0.10.0 and ue5-dev:
// the Codon geometry follows whichever LibCarla the library is built from,
// as the official Python API built from the same sources does.
#include "internal.hpp"

using namespace tsc;

namespace {

template <typename M>
void copy_matrix(const M &m, double *out16) {
  require_ptr(out16, "out16");
  for (size_t i = 0; i < 16; ++i) out16[i] = static_cast<double>(m[i]);
}

}  // namespace

extern "C" {

// No range checks: like the Python API, NaN and infinities just propagate.
tsc_status_t tsc_transform_get_matrix(const tsc_transform_t *transform, double *out16) {
  return TSC_GUARD({
    copy_matrix(to_carla(*require_ptr(transform, "transform")).GetMatrix(), out16);
  });
}

tsc_status_t tsc_transform_get_inverse_matrix(const tsc_transform_t *transform, double *out16) {
  return TSC_GUARD({
    copy_matrix(to_carla(*require_ptr(transform, "transform")).GetInverseMatrix(), out16);
  });
}

}  // extern "C"
