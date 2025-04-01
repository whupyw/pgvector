// MyVector.h
#ifndef MYVector_H
#define MYVector_H

#include <stddef.h> // 为了 size_t
#include <stdint.h> // 为了 uint32_t
// 定义向量结构体
typedef struct
{
    uint32_t *data;   // 存储数据的数组
    size_t size;     // 当前元素数量
    size_t capacity; // 当前容量
} MyVector;

// 初始化向量
void vector_init(MyVector *vec);

void vector_init_with_capacity(MyVector *vec, size_t capacity);

// 插入元素
void vector_push_back(MyVector *vec, uint32_t value);

// 删除最后一个元素
void vector_pop_back(MyVector *vec);

// 获取向量元素
uint32_t vector_get(MyVector *vec, size_t index);

// 设置向量元素
void vector_set(MyVector *vec, size_t index, uint32_t value);

// 清除所有元素，保留内存
void vector_clear(MyVector *vec);

// 清理内存
void vector_free(MyVector *vec);

uint32_t vector_find(MyVector *vec, uint32_t value);

void vector_reserve(MyVector *vec, size_t capacity);

#endif
