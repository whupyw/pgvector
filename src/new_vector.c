#include "new_vector.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void new_vector_init(NewVector *vec, size_t elem_size)
{
    vec->data = malloc(elem_size * 8); // 初始容量为 8
    vec->elem_size = elem_size;
    vec->size = 0;
    vec->capacity = 8;
}

void new_vector_init_with_capacity(NewVector *vec, size_t elem_size, size_t capacity)
{
    vec->data = malloc(elem_size * capacity); // 初始容量为 8
    vec->elem_size = elem_size;
    vec->size = 0;
    vec->capacity = capacity;
}

void new_vector_push_back(NewVector *vec, const void *value)
{
    if (vec->size >= vec->capacity)
    {
        vec->capacity *= 2;
        vec->data = realloc(vec->data, vec->capacity * vec->elem_size);
    }
    memcpy((char *)vec->data + vec->size * vec->elem_size, value, vec->elem_size);
    vec->size++;
}

void new_vector_pop_back(NewVector *vec)
{
    if (vec->size > 0)
        vec->size--;
}

void *new_vector_get(NewVector *vec, size_t index)
{
    assert(index < vec->size);
    return (char *)vec->data + index * vec->elem_size;
}

void new_vector_set(NewVector *vec, size_t index, const void *value)
{
    assert(index < vec->size);
    memcpy((char *)vec->data + index * vec->elem_size, value, vec->elem_size);
}

void new_vector_sort(NewVector *vec, int (*cmp)(const void *, const void *))
{
    qsort(vec->data, vec->size, vec->elem_size, cmp);
}

void new_vector_clear(NewVector *vec)
{
    vec->size = 0;
}

bool new_vector_truncate(NewVector *vec, size_t new_size)
{
    if (new_size >= vec->size)
    {
        // 如果新大小大于或等于当前大小，不做任何改变
        return true;
    }

    // 调整向量的大小
    vec->size = new_size;

    // 可选：如果新大小远小于当前容量，重新分配内存
    void *new_data = realloc(vec->data, new_size * vec->elem_size);
    if (new_data != NULL)
    {
        vec->data = new_data;
        return true;
    }
    else
    {
        // 处理 realloc 失败的情况（可选）
        // 可以记录错误或者保持原有的 data 不变
        return false;
    }
}

void new_vector_reserve(NewVector *vec, size_t new_capacity)
{
    if (new_capacity > vec->capacity)
    {
        // 扩大容量
        //size_t new_capacity = new_capacity;
        void *new_data = realloc(vec->data, new_capacity * vec->elem_size);
        if (new_data)
        {
            vec->data = new_data;
            vec->capacity = new_capacity;
        }
        else
        {
            // 内存分配失败
            return;
        }
    }
    else if (new_capacity < vec->size)
    {
        // 如果新大小小于当前大小，清空多余的元素（可选择不清空）
        // memset((char *)vec->data + new_capacity * vec->elem_size, 0, (vec->size - new_capacity) * vec->elem_size);
        vec->size = new_capacity;
    }
    //
}

void new_vector_resize(NewVector *vec, size_t new_size)
{
    vec->size = new_size;
}

/*
 * Frees the memory allocated for a NewVector and resets its properties.
 * After calling this function, the vector will be empty and its data pointer
 * will be NULL.
 *
 * @param vec  Pointer to the NewVector to be freed
 */
void new_vector_free(NewVector *vec)
{
    free(vec->data);
    vec->data = NULL;
    vec->size = 0;
    vec->capacity = 0;
    vec->elem_size = 0;
}
