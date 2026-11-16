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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    for (int32_t i = 0; i < static_cast<int32_t>(sizeof(pins_) / sizeof(pins_[0])); ++i) {
      pins_[i].count_ = 0;
    }
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  for (int32_t i = 0; i < static_cast<int32_t>(sizeof(pins_) / sizeof(pins_[0])); ++i) {
    pins_[i].reset();
  }
  int ret = OB_SUCCESS;
  is_inited_ = false;
  return ret;
}

int ObFTDictHub::pin_dict_handles(const ObFTDictType type, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  const int32_t type_idx = static_cast<int32_t>(type);
  const int32_t pin_cnt = static_cast<int32_t>(sizeof(pins_) / sizeof(pins_[0]));
  if (type_idx < 0 || type_idx >= pin_cnt) {
    // unsupported dict type, nothing to pin
  } else if (container.get_handles().size() > ObFTDictPins::K_MAX_PINNED_RANGES) {
    STORAGE_FTS_LOG(WARN, "too many dict ranges to pin, skip", K(type));
  } else {
    ObFTDictPins &pins = pins_[type_idx];
    pins.reset();
    int32_t idx = 0;
    for (ObList<ObFTCacheRangeHandle *, ObIAllocator>::const_iterator iter = container.get_handles().begin();
         iter != container.get_handles().end() && idx < ObFTDictPins::K_MAX_PINNED_RANGES;
         ++iter, ++idx) {
      ObFTCacheRangeHandle *handle = *iter;
      if (OB_NOT_NULL(handle) && handle->handle_.is_valid()) {
        pins.handles_[idx].assign(handle->handle_);
      }
    }
    pins.count_ = idx;
  }
  return ret;
}

int ObFTDictHub::unpin_dict_handles(const ObFTDictType type)
{
  int ret = OB_SUCCESS;
  const int32_t type_idx = static_cast<int32_t>(type);
  const int32_t pin_cnt = static_cast<int32_t>(sizeof(pins_) / sizeof(pins_[0]));
  if (type_idx >= 0 && type_idx < pin_cnt) {
    pins_[type_idx].reset();
  }
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        unpin_dict_handles(desc.type_);
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        } else {
          // builtin dicts are immutable, keep their DAT blocks resident so
          // later parser instances load them from cache instead of rebuilding.
          pin_dict_handles(desc.type_, container);
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
