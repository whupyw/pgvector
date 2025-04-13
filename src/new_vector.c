#include "postgres.h"
#include "new_vector.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void new_vector_init(NewVector *vec, size_t elem_size)
{
    size_t the_capacity = 20;
    vec->data = malloc(elem_size * the_capacity); // 初始容量为 8
    vec->elem_size = elem_size;
    vec->size = 0;
    vec->capacity = the_capacity;
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
        // vec->data = realloc(vec->data, vec->capacity * vec->elem_size);
        void *new_data = realloc(vec->data, vec->capacity * vec->elem_size);

        // 如果 realloc 失败，保持原有内存不变
        if (new_data == NULL)
        {
            elog(INFO, "Memory allocation failed during realloc.");
            return;
        }
        vec->data = new_data;
    }
    memcpy((char *)vec->data + vec->size * vec->elem_size, value, vec->elem_size);
    vec->size++;
}

void new_vector_append(NewVector *vec, const NewVector *other)
{
    // 如果其他容器没有元素，则无需执行任何操作
    if (other->size == 0)
    {
        return;
    }

    // 确保当前容器有足够的空间来容纳新元素
    if (vec->size + other->size > vec->capacity)
    {
        new_vector_reserve(vec, vec->size + other->size); // 重新分配空间
    }

    // 将其他容器的元素逐一复制到当前容器的末尾
    memcpy((char *)vec->data + vec->size * vec->elem_size,
           other->data, other->size * vec->elem_size);

    // 更新当前容器的 size
    vec->size += other->size;
}

void new_vector_pop_back(NewVector *vec)
{
    if (vec->size > 0)
        vec->size--;
}

void *new_vector_get(NewVector *vec, size_t index)
{
    if (index >= vec->size)
    {
        elog(INFO, "index: %ld, size: %ld", index, vec->size);
        assert(index < vec->size);
    }

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
    // void *new_data = realloc(vec->data, new_size * vec->elem_size);
    // if (new_data != NULL)
    // {
    //     vec->data = new_data;
    //     return true;
    // }
    // else
    // {
    //     // 处理 realloc 失败的情况（可选）
    //     // 可以记录错误或者保持原有的 data 不变
    //     elog(INFO, "realloc failed");
    //     return false;
    // }
}

void new_vector_reserve(NewVector *vec, size_t new_capacity)
{
    if (new_capacity > vec->capacity)
    {
        // vec->data = realloc(vec->data, vec->capacity * vec->elem_size);
        void *new_data = realloc(new_capacity * vec->elem_size);

        // 如果 alloc 失败，保持原有内存不变
        if (new_data != NULL)
        {
            vec->data = new_data;
            vec->capacity = new_capacity;
        }
    }
    else if (new_capacity < vec->size)
    {
        vec->size = new_capacity;
    }
    //
}

void new_vector_resize(NewVector *vec, size_t new_size)
{
    if (new_size > vec->capacity)
    {
        // 增加容量
        new_vector_reserve(vec, new_size);
        elog(INFO, "realloc");
    }
    assert(new_size <= vec->capacity);
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
    if (vec->data)
    {
        free(vec->data);
        // vec->data = NULL;
    }
    vec->data = NULL;
    vec->size = 0;
    vec->capacity = 0;
    vec->elem_size = 0;
}
