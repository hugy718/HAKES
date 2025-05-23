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

#include <cassert>
#include <cstring>
#include <iostream>

#include "message/keyservice_worker.h"
#include "message/searchservice.h"
#include "search-worker/common/checkpoint.h"
#include "search-worker/common/workerImpl.h"
#include "search-worker/index/ext/HakesFlatIndex.h"
#include "search-worker/index/ext/HakesIndex.h"
#include "search-worker/index/ext/IndexFlatL.h"
#include "search-worker/index/ext/IndexIVFPQFastScanL.h"
#include "utils/base64.h"
#include "utils/cache.h"
#include "utils/fileutil.h"
#include "utils/hexutil.h"
#include "utils/io.h"

#ifdef USE_SGX
#include "Enclave_t.h"
#include "ratls-channel/common/channel_client.h"
#include "utils/tcrypto_ext.h"
#else
#include <filesystem>
#endif  // USE_SGX

namespace {

// to simulate the encryption and decryption overhead in SGX setting
#ifdef USE_SGX
thread_local char user_id_to_use[100];
thread_local size_t user_id_to_use_len;

thread_local uint8_t input_key_buf[BUFSIZ];
thread_local size_t input_key_size;
hakes::SimpleCache<std::string> key_cache{1};
thread_local bool key_cache_release_needed = false;
ratls::RatlsChannelClient* channel = nullptr;

inline std::string GetKeyCacheKey() {
  if (user_id_to_use_len == 0) {
    return "";
  }
  return std::string(user_id_to_use, user_id_to_use_len);
  ;
}

int decode_decryption_keys(const std::string& ks_response) {
  // decode keys from reply message of key service
  std::string user_id;
  std::string input_key_b64;
  std::string model_id;
  std::string model_key_b64;
  int ret = hakes::DecodeKeyServiceWorkerReply(
      ks_response, &user_id, &input_key_b64, &model_id, &model_key_b64);

  // base64 decode the keys
  if (ret != 0) return ret;
  auto input_key_bytes = hakes::base64_decode(
      (const uint8_t*)input_key_b64.data(), input_key_b64.size());

  if ((user_id.size() != user_id_to_use_len) ||
      (memcmp(user_id.c_str(), user_id_to_use, user_id_to_use_len) != 0)) {
    ocall_debug_print_string("unmatched expected id");
    return -1;
  }

  // cache into local execution context
  assert(input_key_bytes.size() < BUFSIZ);
  memset(input_key_buf, 0, input_key_bytes.size() + 1);
  memcpy(input_key_buf, input_key_bytes.c_str(), input_key_bytes.size());
  input_key_size = input_key_bytes.size();

  // store the keys in cache
  key_cache.AddToCache(GetKeyCacheKey(),
                       std::unique_ptr<std::string>(
                           new std::string(std::move(input_key_bytes))));
  return ret;
}

void ClearRequestContext() {
  memset(input_key_buf, 0, BUFSIZ);
  // key checked ensure that id to use are populated
  if (key_cache_release_needed) {
    assert(user_id_to_use_len > 0);
    key_cache.Release(GetKeyCacheKey());
    key_cache_release_needed = false;
  }
  user_id_to_use_len = 0;
}

inline void printf(const char* msg) { ocall_print_string(msg); }

sgx_status_t fetch_from_ks(const std::string& request,
                           const std::string& ks_addr, uint16_t ks_port,
                           std::string* ks_response) {
  assert(ks_response);
  if (!channel) {
    channel = new ratls::RatlsChannelClient{ks_port, std::move(ks_addr),
                                            ratls::CheckContext{}};
    if (channel->Initialize() == -1) {
      printf("failed to setup ratls ctx\n");
      return SGX_ERROR_SERVICE_UNAVAILABLE;
    }
  }
  if (channel->Connect() == -1) {
    printf("failed to setup ratls connection\n");
    return SGX_ERROR_SERVICE_UNAVAILABLE;
  }
  if (channel->Send(std::move(request)) == -1) {
    printf("failed to send request to key service\n");
    channel->Close();
    return SGX_ERROR_UNEXPECTED;
  };

  if (channel->Read(ks_response) == -1) {
    printf("failed to get keys from key service\n");
    ks_response->empty();
    channel->Close();
    return SGX_ERROR_UNEXPECTED;
  }
  channel->CloseConnection();
  return SGX_SUCCESS;
}
#endif  // USE_SGX

#ifdef USE_SGX
std::unique_ptr<float[]> decode_hex_floats(const std::string& vecs_str,
                                           size_t* count,
                                           const std::string& ks_addr,
                                           uint16_t ks_port) {
  // ready key
  int cache_ret = -1;
  do {
    cache_ret = key_cache.CheckAndTakeRef(GetKeyCacheKey());
  } while (cache_ret == -1);
  key_cache_release_needed = true;
  if (cache_ret == 0) {
    // fetch key from ks
    std::string ks_response;
    auto create_key_request_msg = [&]() -> std::string {
      return hakes::GetKeyRequest(
                 std::string(user_id_to_use, user_id_to_use_len), "")
          .EncodeTo();
    };
    auto status =
        fetch_from_ks(create_key_request_msg(), ks_addr, ks_port, &ks_response);
    if (status != SGX_SUCCESS) return nullptr;
    auto dec_ret = decode_decryption_keys(std::move(ks_response));
    if (dec_ret != 0) {
      ClearRequestContext();
      return nullptr;
    }
  } else {
    // get from cache
    std::string* ret = nullptr;
    do {
      ret = key_cache.RetrieveFromCache(GetKeyCacheKey());
    } while (!ret);
    memcpy(input_key_buf, ret->data(), ret->size());
    input_key_size = ret->size();
  }
  key_cache.Release(GetKeyCacheKey());
  key_cache_release_needed = false;

  auto vecs_bytes = hakes::hex_decode(vecs_str.c_str(), vecs_str.size());
  uint8_t* decrypted_vecs = nullptr;
  auto decrypted_size = hakes::get_aes_decrypted_size(vecs_bytes.size());
  sgx_status_t success = hakes::decrypt_content_with_key_aes(
      (const uint8_t*)vecs_bytes.data(), vecs_bytes.size(), input_key_buf,
      &decrypted_vecs);
  if (success != SGX_SUCCESS) {
    return nullptr;
  } else {
    *count = decrypted_size / sizeof(float);
    auto vecs =
        std::unique_ptr<float[]>(new float[decrypted_size / sizeof(float)]);
    std::memcpy(vecs.get(), decrypted_vecs, decrypted_size);
    free(decrypted_vecs);
    return vecs;
  }
}
#endif  // USE_SGX

std::string encode_hex_floats(const float* vecs, size_t count) {
#ifndef USE_SGX
  return hakes::encode_hex_floats(vecs, count);
#else   // USE_SGX
  uint8_t* encrypted_vecs = nullptr;
  size_t encrypted_size = hakes::get_aes_encrypted_size(count * sizeof(float));
  hakes::encrypt_content_with_key_aes((const uint8_t*)vecs,
                                      count * sizeof(float), input_key_buf,
                                      &encrypted_vecs);
  std::string ret = hakes::hex_encode(encrypted_vecs, encrypted_size);
  free(encrypted_vecs);
  return ret;
#endif  // USE_SGX
}

}  // anonymous namespace

namespace search_worker {

bool WorkerImpl::Initialize(bool keep_pa, int cluster_size, int server_id,
                            const std::string& path) {
  cluster_size_ = cluster_size;
  server_id_ = server_id;
  base_path_ = path;
  return true;
}

bool WorkerImpl::IsInitialized() { return true; }

bool WorkerImpl::HasLoadedCollection(const std::string& collection_name) {
  pthread_rwlock_rdlock(&collection_mu_);
  auto loaded = collections.find(collection_name) != collections.end();
  pthread_rwlock_unlock(&collection_mu_);
  return loaded;
}

faiss::HakesCollection* WorkerImpl::GetCollection(
    const std::string& collection_name) {
  pthread_rwlock_rdlock(&collection_mu_);
  auto it = collections.find(collection_name);
  if (it == collections.end() || !it->second->is_loaded()) {
    pthread_rwlock_unlock(&collection_mu_);
    return nullptr;
  }
  auto index = it->second.get();
  pthread_rwlock_unlock(&collection_mu_);
  return index;
}

bool WorkerImpl::LoadCollection(const char* req, size_t req_len, char* resp,
                                size_t resp_len) {
  // decode the message
  hakes::SearchWorkerLoadRequest load_req;
  hakes::SearchWorkerLoadResponse load_resp;
  if (!hakes::decode_search_worker_load_request(std::string{req, req_len},
                                                &load_req)) {
    return false;
  }

  auto collection_name = load_req.collection_name;
  auto loaded = HasLoadedCollection(collection_name);
  faiss::HakesCollection* index = nullptr;
  if (!loaded) {
    pthread_rwlock_wrlock(&collection_mu_);
    if (collections.find(collection_name) == collections.end()) {
      collections[collection_name] =
          std::unique_ptr<faiss::HakesCollection>(new faiss::HakesIndex());
    }
    index = collections[collection_name].get();
    pthread_rwlock_unlock(&collection_mu_);
    auto collection_path = base_path_ + "/" + collection_name;
    auto checkpoint = hakes::get_latest_checkpoint_path(collection_path);
    if (!checkpoint.empty()) {
      loaded = index->Initialize(collection_path + "/" + checkpoint,
                                 load_req.mode, false);
      if (loaded) {
        index->set_loaded();
      }
      auto index_version = hakes::get_checkpoint_no(checkpoint);
      index->index_version_.store(index_version, std::memory_order_relaxed);
    } else {
      load_resp.msg = "checkpoint not found";
    }
  }
  if (!loaded) {
    load_resp.status = false;
    load_resp.msg = "load error: " + load_resp.msg;
    load_resp.aux = "load error";
  } else {
    load_resp.status = true;
    load_resp.msg =
        "load success: " + collection_name + "{" + index->to_string() + "}";
    load_resp.aux = "load success";
  }

  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_load_response(load_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  return load_resp.status;
}

bool WorkerImpl::AddWithIds(const char* req, size_t req_len, char* resp,
                            size_t resp_len) {
  // decode the message
  hakes::SearchWorkerAddRequest add_req;
  hakes::SearchWorkerAddResponse add_resp;
  if (!hakes::decode_search_worker_add_request(std::string{req, req_len},
                                               &add_req)) {
    return false;
  }

  std::unique_ptr<faiss::idx_t[]> assign =
      std::unique_ptr<faiss::idx_t[]>(new faiss::idx_t[1]);
  int vecs_t_d = 0;
  std::unique_ptr<float[]> transformed_vecs;

  // parse collection_name
  auto collection_name = add_req.collection_name;
  auto index = GetCollection(collection_name);
  if (index == nullptr) {
    add_resp.status = false;
    add_resp.msg = "collection is not loaded";
    std::string encoded_response =
        hakes::encode_search_worker_add_response(add_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }
#ifndef USE_SGX
#if DEBUG
  std::cout << "index: " << index->to_string() << std::endl;
  printf("Add request: %s\n", req);
#endif  // DEBUG
#endif  // USE_SGX

  // parse ids and vecs
  size_t parsed_count;
  auto ids = hakes::decode_hex_int64s(add_req.ids, &parsed_count);
  assert(parsed_count == 1);
#ifndef USE_SGX
  auto vecs = hakes::decode_hex_floats(add_req.vecs, &parsed_count);
#else   // USE_SGX
  memcpy(user_id_to_use, add_req.user_id.c_str(), add_req.user_id.size());
  user_id_to_use_len = add_req.user_id.size();
  auto vecs = decode_hex_floats(add_req.vecs, &parsed_count, add_req.ks_addr,
                                add_req.ks_port);
#endif  // USE_SGX
  assert(parsed_count == add_req.d);
  bool success =
      (ids[0] % cluster_size_ == server_id_)
          ? index->AddWithIds(1, add_req.d, vecs.get(), ids.get(), assign.get(),
                              &vecs_t_d, &transformed_vecs)
          : index->AddBase(1, add_req.d, vecs.get(), ids.get());

  if (!success) {
    add_resp.status = false;
    add_resp.msg = "add error";
    add_resp.aux = "add error";
  } else {
    add_resp.status = true;
    add_resp.msg = "add success";
    add_resp.aux = "add success";
  }

  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_add_response(add_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  // printf("-> %ld\n", index->ntotal);
  return add_resp.status;
}

bool WorkerImpl::Search(const char* req, size_t req_len, char* resp,
                        size_t resp_len) {
  hakes::SearchWorkerSearchResponse search_resp;
  hakes::SearchWorkerSearchRequest search_req;
  if (!hakes::decode_search_worker_search_request(std::string{req, req_len},
                                                  &search_req)) {
    search_resp.status = false;
    search_resp.msg = "decode search request error";
    std::string encoded_response =
        hakes::encode_search_worker_search_response(search_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }

  // parse collection_name
  auto collection_name = search_req.collection_name;
  auto index = GetCollection(collection_name);
  if (index == nullptr) {
    search_resp.status = false;
    search_resp.msg = "collection does not exist error";
    std::string encoded_response =
        hakes::encode_search_worker_search_response(search_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }
#ifndef USE_SGX
#if DEBUG
  std::cout << "index: " << index->to_string() << std::endl;
  printf("Search request: %s\n", req);
#endif  // DEBUG
#endif  // USE_SGX

  // parse ids and vecs
  size_t parsed_count;
#ifndef USE_SGX
  auto vecs = hakes::decode_hex_floats(search_req.vecs, &parsed_count);
#else   // USE_SGX
  ocall_debug_print_string(search_req.user_id.c_str());
  memcpy(user_id_to_use, search_req.user_id.c_str(), search_req.user_id.size());
  user_id_to_use_len = search_req.user_id.size();
  auto vecs = decode_hex_floats(search_req.vecs, &parsed_count,
                                search_req.ks_addr, search_req.ks_port);
#endif  // USE_SGX
  assert(parsed_count == search_req.d);

  std::unique_ptr<float[]> scores;
  std::unique_ptr<faiss::idx_t[]> ids;
  auto params = faiss::HakesSearchParams{search_req.nprobe, search_req.k,
                                         search_req.k_factor,
                                         faiss::METRIC_INNER_PRODUCT};
  bool success =
      index->Search(1, search_req.d, vecs.get(), params, &scores, &ids);

  if (!success) {
    search_resp.status = false;
    search_resp.msg = "search error";
    search_resp.ids = "";
    search_resp.scores = "";
    search_resp.aux = "search error";
  } else {
    search_resp.status = true;
    search_resp.msg = "search success";
    search_resp.ids =
        hakes::encode_hex_int64s(ids.get(), search_req.k * search_req.k_factor);
    search_resp.scores = hakes::encode_hex_floats(
        scores.get(), search_req.k * search_req.k_factor);
    search_resp.aux = "search success";
  }
  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_search_response(search_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  return success;
}

bool WorkerImpl::Rerank(const char* req, size_t req_len, char* resp,
                        size_t resp_len) {
  hakes::SearchWorkerRerankResponse rerank_resp;
  hakes::SearchWorkerRerankRequest rerank_req;
  if (!hakes::decode_search_worker_rerank_request(std::string{req, req_len},
                                                  &rerank_req)) {
    rerank_resp.status = false;
    rerank_resp.msg = "decode search request error";
    rerank_resp.ids = "";
    rerank_resp.scores = "";
    rerank_resp.aux = "decode search request error";
    std::string encoded_response =
        hakes::encode_search_worker_rerank_response(rerank_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }

  // parse collection_name
  auto collection_name = rerank_req.collection_name;
  auto index = GetCollection(collection_name);
  if (index == nullptr) {
    rerank_resp.status = false;
    rerank_resp.msg = "collection does not exist error";
    std::string encoded_response =
        hakes::encode_search_worker_rerank_response(rerank_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }
#ifndef USE_SGX
#if DEBUG
  std::cout << "index: " << index->to_string() << std::endl;
  printf("Rerank request: %s\n", req);
#endif  // DEBUG
#endif  // USE_SGX

  // parse ids
  size_t parsed_count;
#ifndef USE_SGX
  auto vecs = hakes::decode_hex_floats(rerank_req.vecs, &parsed_count);
#else   // USE_SGX
  memcpy(user_id_to_use, rerank_req.user_id.c_str(), rerank_req.user_id.size());
  user_id_to_use_len = rerank_req.user_id.size();
  auto vecs = decode_hex_floats(rerank_req.vecs, &parsed_count,
                                rerank_req.ks_addr, rerank_req.ks_port);
#endif  // USE_SGX
  assert(parsed_count == rerank_req.d);
  auto input_ids =
      hakes::decode_hex_int64s(rerank_req.input_ids, &parsed_count);

  // clean the ids to remove -1
  auto valid_count = 0;
  for (int i = 0; i < parsed_count; i++) {
    if (input_ids[i] != -1 && input_ids[i] % cluster_size_ == server_id_) {
      input_ids[valid_count] = input_ids[i];
      valid_count++;
    }
  }

  std::unique_ptr<float[]> scores;
  std::unique_ptr<faiss::idx_t[]> ids;

#ifndef USE_SGX
#if DEBUG
  // print the rerank request data
  printf("Rerank request: n=%d, d=%d, k=%d, metric_type=%d\n", 1, rerank_req.d,
         rerank_req.k, rerank_req.metric_type);
  for (int j = 0; j < valid_count; j++) {
    printf("base label: %ld", input_ids[j]);
  }
#endif  // DEBUG
#endif  // USE_SGX

  faiss::idx_t k_base_count = (faiss::idx_t)valid_count;
  std::unique_ptr<float[]> base_distances =
      std::unique_ptr<float[]>(new float[k_base_count]);
  std::memset(base_distances.get(), 0, sizeof(float) * k_base_count);
  bool success =
      index->Rerank(1, rerank_req.d, vecs.get(), rerank_req.k, &k_base_count,
                    input_ids.get(), base_distances.get(), &scores, &ids);

  if (!success) {
    rerank_resp.status = false;
    rerank_resp.msg = "rerank error";
    rerank_resp.ids = "";
    rerank_resp.scores = "";
    rerank_resp.aux = "rerank error";
  } else {
    rerank_resp.status = true;
    rerank_resp.msg = "rerank success";
    rerank_resp.ids = hakes::encode_hex_int64s(ids.get(), rerank_req.k);
    rerank_resp.scores = encode_hex_floats(scores.get(), rerank_req.k);
    rerank_resp.aux = "rerank success";
  }
  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_rerank_response(rerank_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  return success;
}

bool WorkerImpl::Delete(const char* req, size_t req_len, char* resp,
                        size_t resp_len) {
  // decode the message
  hakes::SearchWorkerDeleteRequest delete_req;
  hakes::SearchWorkerDeleteResponse delete_resp;
  if (!hakes::decode_search_worker_delete_request(std::string{req, req_len},
                                                  &delete_req)) {
    return false;
  }

  // parse collection_name
  auto collection_name = delete_req.collection_name;
  auto index = GetCollection(collection_name);
  if (index == nullptr) {
    delete_resp.status = false;
    delete_resp.msg = "collection does not exist error";
    std::string encoded_response =
        hakes::encode_search_worker_delete_response(delete_resp);
    assert(encoded_response.size() < resp_len);
    memcpy(resp, encoded_response.c_str(), encoded_response.size());
    resp[encoded_response.size()] = '\0';
    return false;
  }

  // parse ids and vecs
  size_t parsed_count;
  auto ids = hakes::decode_hex_int64s(delete_req.ids, &parsed_count);
  bool success = index->DeleteWithIds(parsed_count, ids.get());
  if (!success) {
    delete_resp.status = false;
    delete_resp.msg = "delete error";
    delete_resp.aux = "delete error";
  } else {
    delete_resp.status = true;
    delete_resp.msg = "delete success";
    delete_resp.aux = "delete success";
  }

  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_delete_response(delete_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  return delete_resp.status;
}

bool WorkerImpl::Checkpoint(const char* req, size_t req_len, char* resp,
                            size_t resp_len) {
  hakes::SearchWorkerCheckpointRequest checkpoint_req;
  hakes::SearchWorkerCheckpointResponse checkpoint_resp;
  if (!hakes::decode_search_worker_checkpoint_request(std::string{req, req_len},
                                                      &checkpoint_req)) {
    return false;
  }

  // parse collection_name
  auto collection_name = checkpoint_req.collection_name;
  auto index = GetCollection(collection_name);
  bool success = false;
  if (index == nullptr) {
    checkpoint_resp.status = false;
    checkpoint_resp.msg = "collection does not exist error";
  } else {
    // checkpoint the index
    std::string checkpoint_path =
        base_path_ + "/" + collection_name + "/" +
        hakes::format_checkpoint_path(
            index->index_version_.fetch_add(1, std::memory_order_relaxed) + 1);
    std::filesystem::create_directories(checkpoint_path);
    std::filesystem::permissions(checkpoint_path,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_all |
                                     std::filesystem::perms::others_all,
                                 std::filesystem::perm_options::add);
    success = index->Checkpoint(checkpoint_path);
  }

  if (!success) {
    checkpoint_resp.status = false;
    checkpoint_resp.msg = "checkpoint error: " + checkpoint_resp.msg;
    checkpoint_resp.aux = "checkpoint error";
  } else {
    checkpoint_resp.status = true;
    checkpoint_resp.msg = "checkpoint success";
    checkpoint_resp.aux = "checkpoint success";
  }

  // encode response
  std::string encoded_response =
      hakes::encode_search_worker_checkpoint_response(checkpoint_resp);
  assert(encoded_response.size() < resp_len);
  memcpy(resp, encoded_response.c_str(), encoded_response.size());
  resp[encoded_response.size()] = '\0';

  return success;
}

bool WorkerImpl::Close() { return true; }

}  // namespace search_worker