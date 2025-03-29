#ifndef MY2DVECTOR_H
#define MY2DVECTOR_H

#include <stddef.h> // size_t
#include <stdint.h>
// 定义二维动态向量结构体
typedef struct
{
    uint32_t **data;  // 存储二维数据的指针
    size_t rows; // 行数
    size_t cols; // 列数
} My2DVector;

// 初始化二维向量
void my2d_vector_init(My2DVector *vec, size_t rows, size_t cols);

// 插入元素
void my2d_vector_set(My2DVector *vec, size_t row, size_t col, uint32_t value);

// 获取元素
uint32_t my2d_vector_get(const My2DVector *vec, size_t row, size_t col);

// 释放内存
void my2d_vector_free(My2DVector *vec);

#endif
