#ifndef VECTOR_CACHE_H
#define VECTOR_CACHE_H
#include "vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "my_vector.h"

typedef struct
{
    uint32_t vector_id;
    Vector *vector;
    uint32_t *neighbors;
    uint32_t neighbor_count;
    // uint32_t dim;
} VectorCache;

bool init_vector_cache(VectorCache *cache);
void free_vector_cache(VectorCache *cache);

#endif