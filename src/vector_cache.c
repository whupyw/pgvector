#include "postgres.h"
#include "vector_cache.h"

bool init_vector_cache(VectorCache *cache)
{
    if (cache == NULL)
    {
        return false; // 返回错误，指针为 NULL
    }

    cache->vector = (Vector *)palloc(sizeof(Vector));
    if (cache->vector == NULL)
    {
        return -1; // 内存分配失败
    }

    cache->neighbors = (MyVector *)palloc(sizeof(MyVector));
    vector_init(cache->neighbors);
}

// 释放 VectorCache 中的资源
void free_vector_cache(VectorCache *cache)
{
    if (cache != NULL)
    {
        if (cache->vector != NULL)
        {
            pfree(cache->vector);
        }
        if (cache->neighbors != NULL)
        {
            vector_free(cache->neighbors);
            pfree(cache->neighbors);
        }
    }
}