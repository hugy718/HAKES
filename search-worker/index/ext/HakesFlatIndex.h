/*
 * Copyright 2024 The HAKES Authors
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

#ifndef HAKES_SEARCHWORKER_INDEX_EXT_HAKESFLATINDEX_H_
#define HAKES_SEARCHWORKER_INDEX_EXT_HAKESFLATINDEX_H_

#include "search-worker/index/ext/HakesCollection.h"
#include "search-worker/index/ext/IndexFlatL.h"
#include "search-worker/index/ext/TagChecker.h"

namespace faiss {

class HakesFlatIndex : public HakesCollection {
 public:
  HakesFlatIndex(int d, faiss::MetricType metric) {
    refine_index_.reset(new faiss::IndexFlatL(d, metric));
    del_checker_.reset(new TagChecker<idx_t>());
  }
  ~HakesFlatIndex() = default;

  // delete copy constructors and assignment operators
  HakesFlatIndex(const HakesFlatIndex&) = delete;
  HakesFlatIndex& operator=(const HakesFlatIndex&) = delete;
  // delete move constructors and assignment operators
  HakesFlatIndex(HakesFlatIndex&&) = delete;
  HakesFlatIndex& operator=(HakesFlatIndex&&) = delete;

  bool Initialize(const std::string& path, int mode = 0,
                  bool keep_pa = false) override;

  void UpdateIndex(const HakesCollection* other) override { return; }

  // it is assumed that receiving engine shall store the full vecs of all
  // inputs.
  bool AddWithIds(int n, int d, const float* vecs, const faiss::idx_t* ids,
                  faiss::idx_t* assign, int* vecs_t_d,
                  std::unique_ptr<float[]>* vecs_t) override;

  bool AddBase(int n, int d, const float* vecs,
               const faiss::idx_t* ids) override {
    // noop. We may add a quantized flat index later
    return true;
  }

  bool AddRefine(int n, int d, const float* vecs,
                 const faiss::idx_t* ids) override {
    return AddWithIds(n, d, vecs, ids, nullptr, nullptr, nullptr);
  }

  bool Search(int n, int d, const float* query, const HakesSearchParams& params,
              std::unique_ptr<float[]>* distances,
              std::unique_ptr<faiss::idx_t[]>* labels) override;

  bool Rerank(int n, int d, const float* query, int k,
              faiss::idx_t* k_base_count, faiss::idx_t* base_labels,
              float* base_distances, std::unique_ptr<float[]>* distances,
              std::unique_ptr<faiss::idx_t[]>* labels) override {
    // not implemented
    assert(false);
    return false;
  }

  bool Checkpoint(const std::string& checkpoint_path) const override;

  std::string GetParams() const override { return ""; }

  bool UpdateParams(const std::string& path) override { return true; }

  inline bool DeleteWithIds(int n, const idx_t* ids) override {
    if (del_checker_) {
      del_checker_->set(n, ids);
    }
    return true;
  }

  std::string to_string() const override;

  std::unique_ptr<faiss::IndexFlatL> refine_index_;
  std::unique_ptr<TagChecker<idx_t>> del_checker_;
};

}  // namespace faiss

#endif  // HAKES_SEARCHWORKER_INDEX_EXT_HAKESFLATINDEX_H_
