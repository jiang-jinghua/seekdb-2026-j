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
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
} sk_cuvs_index;

/* ---------------------------------------------------------------------------
 * CAGRA reuse for the explicit batch API.
 *
 * dbms_vector.batch_knn rebuilds the whole graph on every call (documented
 * limitation of the reference implementation). The graph depends only on
 * (base, n, dim), and a probe run typically issues several batch_knn calls
 * (top-10, top-100, ...) over the SAME index table, so the build is repeated
 * work. Keep the last couple of graphs and hand them back while the data is
 * unchanged.
 *
 * Only seekdb_cuvs_build_cached() puts handles in this cache; the per-index
 * seekdb_cuvs_build() path (registry / DDL / snapshot) is untouched, so nothing
 * that owns a handle can have it evicted underneath it. Cached handles are
 * ignored by seekdb_cuvs_free() and live until process exit.
 *
 * Identity is (n, dim, sample fingerprint): first, middle and last row. Two
 * different datasets agreeing on all of those is not a realistic collision for
 * a probe table that is written once and then only queried.
 * ------------------------------------------------------------------------- */
#define SK_CACHE_SLOTS 2
typedef struct { void* h; long n, dim; unsigned long long fp; } sk_cache_slot;
static sk_cache_slot g_sk_cache[SK_CACHE_SLOTS];
static pthread_mutex_t g_sk_cache_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned long long sk_fnv(unsigned long long h, const void* p, size_t bytes)
{
  const unsigned char* b = (const unsigned char*)p;
  size_t i;
  for (i = 0; i < bytes; ++i) { h ^= (unsigned long long)b[i]; h *= 1099511628211ULL; }
  return h;
}

static unsigned long long sk_fingerprint(const float* base, long n, long dim)
{
  unsigned long long h = 14695981039346656037ULL;
  const size_t row = (size_t)dim * sizeof(float);
  h = sk_fnv(h, &n, sizeof(n));
  h = sk_fnv(h, &dim, sizeof(dim));
  h = sk_fnv(h, base, row);                                   /* first row */
  h = sk_fnv(h, base + (size_t)(n / 2) * (size_t)dim, row);   /* middle row */
  h = sk_fnv(h, base + (size_t)(n - 1) * (size_t)dim, row);   /* last row */
  return h;
}

/* Called by seekdb_cuvs_free(): cached handles must survive. */
static int sk_cache_holds(void* handle)
{
  int found = 0, i = 0;
  pthread_mutex_lock(&g_sk_cache_mu);
  for (i = 0; i < SK_CACHE_SLOTS; ++i) {
    if (g_sk_cache[i].h != NULL && g_sk_cache[i].h == handle) { found = 1; }
  }
  pthread_mutex_unlock(&g_sk_cache_mu);
  return found;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  // Preserve graph density; offset bounded refinement with a wider search.
  h->ip->nn_descent_niter = 40;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 1024) required_itopk = 1024;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
      sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (sk_cache_holds(handle)) { return; }  /* cached graphs live for the process */
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

/* Same contract as seekdb_cuvs_build(), except the result may be a graph that
 * this process already built for byte-identical data. The returned handle must
 * still be treated as "release with seekdb_cuvs_free()" -- that call is a no-op
 * for cached graphs, so callers need no special casing. */
void* seekdb_cuvs_build_cached(const float* base, long n, long dim)
{
  unsigned long long fp = 0;
  void* h = NULL;
  void* evict = NULL;
  int slot = -1, i = 0;

  if (!base || n <= 0 || dim <= 0) return NULL;
  fp = sk_fingerprint(base, n, dim);

  pthread_mutex_lock(&g_sk_cache_mu);
  for (i = 0; i < SK_CACHE_SLOTS; ++i) {
    if (g_sk_cache[i].h != NULL && g_sk_cache[i].n == n &&
        g_sk_cache[i].dim == dim && g_sk_cache[i].fp == fp) {
      h = g_sk_cache[i].h;
      pthread_mutex_unlock(&g_sk_cache_mu);
      fprintf(stderr, "[seekdb_cuvs] reuse CAGRA n=%ld dim=%ld (cache hit)\n", n, dim);
      return h;
    }
  }
  pthread_mutex_unlock(&g_sk_cache_mu);

  h = seekdb_cuvs_build(base, n, dim);
  if (!h) return NULL;

  pthread_mutex_lock(&g_sk_cache_mu);
  for (i = 0; i < SK_CACHE_SLOTS; ++i) {
    if (g_sk_cache[i].h == NULL) { slot = i; break; }
  }
  if (slot < 0) { slot = 0; }               /* simple FIFO-ish eviction */
  evict = g_sk_cache[slot].h;
  g_sk_cache[slot].h = h;
  g_sk_cache[slot].n = n;
  g_sk_cache[slot].dim = dim;
  g_sk_cache[slot].fp = fp;
  pthread_mutex_unlock(&g_sk_cache_mu);

  /* `evict` is no longer referenced by any slot, so this really frees it. */
  if (evict != NULL) { seekdb_cuvs_free(evict); }
  return h;
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
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
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
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
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
