#ifndef NEWVECTOR_H
#define NEWVECTOR_H

#include <stddef.h>
#include <stdbool.h>

typedef struct
{
    void *data;       // 任意类型的数据
    size_t elem_size; // 每个元素大小（如 sizeof(Neighbor)）
    size_t size;      // 当前元素数量
    size_t capacity;  // 分配的元素数量
} NewVector;

// 初始化
void new_vector_init(NewVector *vec, size_t elem_size);

void new_vector_init_with_capacity(NewVector *vec, size_t elem_size, size_t capacity);

// 添加元素
void new_vector_push_back(NewVector *vec, const void *value);

// 删除最后一个元素
void new_vector_pop_back(NewVector *vec);

// 获取元素指针（只读）
void *new_vector_get(NewVector *vec, size_t index);

// 设置元素
void new_vector_set(NewVector *vec, size_t index, const void *value);

// 排序（传入比较函数）
void new_vector_sort(NewVector *vec, int (*cmp)(const void *, const void *));

void new_vector_clear(NewVector *vec);

void new_vector_reserve(NewVector *vec, size_t new_size);

void new_vector_resize(NewVector *vec, size_t new_size);

// 释放资源
void new_vector_free(NewVector *vec);

bool new_vector_truncate(NewVector *vec, size_t new_size);

#endif
