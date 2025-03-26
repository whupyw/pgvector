// MyVector.h
#ifndef MYVector_H
#define MYVector_H

#include <stddef.h> // 为了 size_t

// 定义向量结构体
typedef struct
{
    int *data;       // 存储数据的数组
    size_t size;     // 当前元素数量
    size_t capacity; // 当前容量
} MyVector;

// 初始化向量
void vector_init(MyVector *vec);

// 插入元素
void vector_push_back(MyVector *vec, int value);

// 删除最后一个元素
void vector_pop_back(MyVector *vec);

// 获取向量元素
int vector_get(MyVector *vec, size_t index);

// 设置向量元素
void vector_set(MyVector *vec, size_t index, int value);

// 清理内存
void vector_free(MyVector *vec);

#endif
