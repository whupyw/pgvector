#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "my_two_d_vector.h"
#include <stdint.h>

// 初始化二维向量
void my2d_vector_init(My2DVector *vec, size_t rows, size_t cols)
{
    vec->rows = rows;
    vec->cols = cols;

    // 动态分配内存：分配行指针数组
    vec->data = (uint32_t **)malloc(rows * sizeof(uint32_t *));
    assert(vec->data != NULL);

    // 为每一行分配列空间
    for (size_t i = 0; i < rows; ++i)
    {
        vec->data[i] = (uint32_t *)malloc(cols * sizeof(uint32_t));
        assert(vec->data[i] != NULL);
    }

    // 初始化二维数组的值为 0（可选）
    for (size_t i = 0; i < rows; ++i)
    {
        for (size_t j = 0; j < cols; ++j)
        {
            vec->data[i][j] = 0;
        }
    }
}

// 设置元素值
void my2d_vector_set(My2DVector *vec, size_t row, size_t col, uint32_t value)
{
    assert(row < vec->rows && col < vec->cols); // 确保索引在有效范围内
    vec->data[row][col] = value;
}

// 获取元素值
uint32_t my2d_vector_get(const My2DVector *vec, size_t row, size_t col)
{
    assert(row < vec->rows && col < vec->cols); // 确保索引在有效范围内
    return vec->data[row][col];
}

// 释放内存
void my2d_vector_free(My2DVector *vec)
{
    for (size_t i = 0; i < vec->rows; ++i)
    {
        free(vec->data[i]); // 释放每一行的内存
    }
    free(vec->data); // 释放存储行指针的内存
    vec->data = NULL;
}
