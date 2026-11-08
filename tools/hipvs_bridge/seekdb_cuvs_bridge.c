/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Shared cuVS resource.
 *
 * cuvsResourcesCreate() builds a RAFT handle: device stream, cuBLAS/rocBLAS
 * handles and the RMM memory pool. That costs O(10ms) and used to be paid on
 * every build (and every per-call batch_knn). The handle is stateless with
 * respect to a given index, so keep one process-wide instance instead; it also
 * lets the RMM pool warm up and serve later allocations without cudaMalloc.
 *
 * Access is serialized by the caller (ob_vsag_adaptor holds g_ob_cuvs_mu across
 * the whole build/search job and joins the worker thread), so a single handle
 * is safe. The mutex here only protects lazy creation.
 * ------------------------------------------------------------------------- */
static cuvsResources_t g_sk_res = NULL;
static pthread_mutex_t g_sk_res_mu = PTHREAD_MUTEX_INITIALIZER;

static cuvsResources_t sk_resource(void)
{
  cuvsResources_t res = NULL;
  pthread_mutex_lock(&g_sk_res_mu);
  if (g_sk_res == NULL) {
    if (cuvsResourcesCreate(&g_sk_res) != CUVS_SUCCESS) { g_sk_res = NULL; }
  }
  res = g_sk_res;
  pthread_mutex_unlock(&g_sk_res_mu);
  return res;
}

typedef struct {
  cuvsResources_t         res;      /* borrowed from sk_resource(); never destroyed here */
  cuvsCagraIndexParams_t  ip;
  cuvsCagraIndex_t        idx;
  cuvsCagraSearchParams_t sp;       /* created once, reused for every query */
  /* Reused device scratch. Reallocating these per query cost 3 pool round-trips
   * plus a fresh search-param object for what is a 0.5ms GPU kernel. */
  float*    qbuf;  size_t qbuf_cap;
  unsigned* obuf;  size_t obuf_cap;
  float*    dbuf;  size_t dbuf_cap;
  long n, dim;
} sk_cuvs_index;

/* Grow (never shrink) a device buffer to at least `need` bytes. */
static int sk_ensure(void* res_, void** buf, size_t* cap, size_t need)
{
  cuvsResources_t res = (cuvsResources_t)res_;
  if (*buf != NULL && *cap >= need) { return 0; }
  if (*buf != NULL) {
    cuvsRMMFree(res, *buf, *cap);
    *buf = NULL;
    *cap = 0;
  }
  if (cuvsRMMAlloc(res, buf, need) != CUVS_SUCCESS) {
    *buf = NULL;
    *cap = 0;
    return 1;
  }
  *cap = need;
  return 0;
}

static void sk_release_buffers(sk_cuvs_index* h)
{
  if (h->qbuf) { cuvsRMMFree(h->res, h->qbuf, h->qbuf_cap); h->qbuf = NULL; h->qbuf_cap = 0; }
  if (h->obuf) { cuvsRMMFree(h->res, h->obuf, h->obuf_cap); h->obuf = NULL; h->obuf_cap = 0; }
  if (h->dbuf) { cuvsRMMFree(h->res, h->dbuf, h->dbuf_cap); h->dbuf = NULL; h->dbuf_cap = 0; }
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->res = sk_resource();
  if (h->res == NULL) { free(h); return NULL; }

  DLManagedTensor dset; int64_t ds[2] = {(int64_t)n, (int64_t)dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || nq <= 0 || topk <= 0) return 1;
  int rc = 0;
  const size_t need_q = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t need_o = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  const size_t need_d = sizeof(float) * (size_t)nq * (size_t)topk;
  cuvsCagraSearchParams_t sp = h->sp;

  if (sk_ensure(h->res, (void**)&h->qbuf, &h->qbuf_cap, need_q)) { rc = 10; goto done; }
  if (sk_ensure(h->res, (void**)&h->obuf, &h->obuf_cap, need_o)) { rc = 11; goto done; }
  if (sk_ensure(h->res, (void**)&h->dbuf, &h->dbuf_cap, need_d)) { rc = 12; goto done; }
  if (cudaMemcpy(h->qbuf, query, need_q, cudaMemcpyDefault) != cudaSuccess) { rc = 13; goto done; }
  {
    DLManagedTensor qt, nt, dt; int64_t qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->qbuf; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->obuf; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dbuf; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (sp == NULL) {
      if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc = 14; goto done; }
      sp = h->sp;
    }
    {
      /* Unchanged from the reference tuning: recall must stay at baseline. */
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->obuf, need_o, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, h->dbuf, need_d, cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_release_buffers(h);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  free(h);  /* res is shared, intentionally not destroyed */
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  index->res = sk_resource();
  if (index->res == NULL) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
  index->idx = NULL;
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
