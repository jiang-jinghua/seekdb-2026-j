/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_SPARSE_RETRIEVAL_UTIL_H_
#define OB_SPARSE_RETRIEVAL_UTIL_H_

#include "common/datum/ob_datum.h"
#include "share/datum/ob_datum_funcs.h"
#include "share/datum/ob_datum_cmp_func_def.h"

namespace oceanbase
{
namespace storage
{

class ObDomainIdCmp
{
public:
  ObDomainIdCmp() : cmp_func_(nullptr), cs_type_(CS_TYPE_INVALID), is_uint64_(false) {}
  ~ObDomainIdCmp() {}
  inline int init(const ObObjMeta &obj_meta);
  // Docid comparisons dominate BMW pivot advancement and range evaluation.
  // Route the common cases (equal-length ASCII strings, unsigned integers)
  // straight to the specialized datum comparison instead of the indirect
  // collation function table on every step.
  inline int compare(const ObDatum &lhs, const ObDatum &rhs, int &cmp_ret) const
  {
    int ret = OB_SUCCESS;
    const bool lmax = lhs.is_max();
    const bool lmin = lhs.is_min();
    const bool rmax = rhs.is_max();
    const bool rmin = rhs.is_min();
    if (OB_UNLIKELY(nullptr == cmp_func_)) {
      ret = OB_NOT_INIT;
      STORAGE_LOG(WARN,"not init", K(ret));
    } else if (lhs.is_ext() || rhs.is_ext()) {
      if (OB_UNLIKELY(!(lmax || lmin || rmax || rmin))) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN,"unexpected ext datum", K(ret), K(lhs), K(rhs));
      } else if (lmax || rmax) {
        cmp_ret = lmax - rmax;
      } else if (lmin || rmin) {
        cmp_ret = -(lmin - rmin);
      } else {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (OB_UNLIKELY(lhs.is_null() || rhs.is_null())) {
      ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
    } else if (OB_LIKELY(is_uint64_)) {
      const uint64_t lv = lhs.get_uint64();
      const uint64_t rv = rhs.get_uint64();
      cmp_ret = lv < rv ? -1 : (lv > rv ? 1 : 0);
    } else if (OB_LIKELY(lhs.len_ == rhs.len_ && nullptr != lhs.ptr_ && nullptr != rhs.ptr_)) {
      if (OB_LIKELY(lhs.ptr_ == rhs.ptr_)) {
        cmp_ret = 0;
      } else if (common::datum_cmp::ob_ascii_cmp_dispatch(lhs, rhs, cs_type_, cmp_ret)) {
        // decided by the ASCII fast path
      } else {
        // non-ASCII bytes involved, let the real collation decide
        ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
      }
    } else {
      ret = cmp_func_(lhs, rhs, cmp_ret, nullptr);
    }
    return ret;
  }
  inline void reset() { cmp_func_ = nullptr; }
private:
  common::ObDatumCmpFuncType cmp_func_;
  common::ObCollationType cs_type_;
  bool is_uint64_;
  DISALLOW_COPY_AND_ASSIGN(ObDomainIdCmp);
};

int ObDomainIdCmp::init(const ObObjMeta &obj_meta)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN,"invalid argument", K(ret), K(obj_meta));
  } else {
    cs_type_ = obj_meta.get_collation_type();
    is_uint64_ = (common::ObUInt64Type == obj_meta.get_type());
    common::ObDatumBasicFuncs *id_basic_funcs = ObDatumFuncs::get_basic_func(
      obj_meta.get_type(), obj_meta.get_collation_type());
    if (OB_ISNULL(id_basic_funcs)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN,"failed to get basic functions", K(ret), K(obj_meta));
    } else {
      cmp_func_ = id_basic_funcs->null_first_cmp_;
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

#endif
