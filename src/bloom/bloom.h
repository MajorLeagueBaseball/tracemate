#ifndef BLOOM_H_C
#define BLOOM_H_C

#include <stdint.h>
#include <stddef.h>

typedef struct bloom bloom_t;

bloom_t *bloom_create(size_t total_items);
void bloom_destroy(bloom_t *b);

void bloom_add(bloom_t *bloom, const char *string, size_t len);
bool bloom_contains(bloom_t *bloom, const char *string, size_t len);

#endif
