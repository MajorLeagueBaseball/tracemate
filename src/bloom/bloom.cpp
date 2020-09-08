extern "C" {
  #include "bloom.h"
}
#define XXH_PRIVATE_API
#include "xxhash.h"

#include "bloom.hpp"

struct bloom {
  bloomfilter::BloomFilter<uint64_t, 16, false> *bloom;
  pthread_mutex_t mutex;
};

extern "C"
bloom_t *
bloom_create(size_t total_items)
{
  bloomfilter::BloomFilter<uint64_t, 16, false> *b = new bloomfilter::BloomFilter<uint64_t, 16, false>(total_items);
  bloom_t *rval = (bloom_t *)calloc(1, sizeof(bloom_t));
  pthread_mutex_init(&rval->mutex, NULL);
  rval->bloom = b;
  return rval;
}

extern "C"
void 
bloom_destroy(bloom_t *b)
{
  pthread_mutex_lock(&b->mutex);
  delete b->bloom;
  pthread_mutex_unlock(&b->mutex);
  pthread_mutex_destroy(&b->mutex);
  free(b);
}

extern "C"
void 
bloom_add(bloom_t *bloom, const char *string, size_t len)
{
  XXH64_hash_t hash = XXH64(string, len, 0);
  pthread_mutex_lock(&bloom->mutex);
  bloom->bloom->Add((uint64_t)hash);
  pthread_mutex_unlock(&bloom->mutex);
}

extern "C"
bool 
bloom_contains(bloom_t *bloom, const char *string, size_t len)
{
  XXH64_hash_t hash = XXH64(string, len, 0);
  pthread_mutex_lock(&bloom->mutex);
  bloomfilter::Status s = bloom->bloom->Contain((uint64_t)hash);
  pthread_mutex_unlock(&bloom->mutex);
  return s == bloomfilter::Ok;
}
