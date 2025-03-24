// vector.c
#include "my_vector.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define VECTOR_INITIAL_CAPACITY 4 // 初始容量

// 初始化向量
void vector_init(MyVector *vec)
{
    vec->size = 0;
    vec->capacity = VECTOR_INITIAL_CAPACITY;
    vec->data = (int *)malloc(vec->capacity * sizeof(int)); // 分配内存
    if (vec->data == NULL)
    {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
}

// 扩展向量容量
void vector_resize(MyVector *vec, size_t new_capacity)
{
    int *new_data = (int *)realloc(vec->data, new_capacity * sizeof(int));
    if (new_data == NULL)
    {
        fprintf(stderr, "Memory reallocation failed\n");
        free(vec->data);
        exit(1);
    }
    vec->data = new_data;
    vec->capacity = new_capacity;
}

// 插入元素
void vector_push_back(MyVector *vec, int value)
{
    if (vec->size == vec->capacity)
    {
        // 如果容量满了，扩展容量
        vector_resize(vec, vec->capacity * 2);
    }
    vec->data[vec->size] = value;
    vec->size++;
}

// 删除最后一个元素
void vector_pop_back(MyVector *vec)
{
    if (vec->size > 0)
    {
        vec->size--;
    }
}

// 获取向量元素
int vector_get(MyVector *vec, size_t index)
{
    if (index >= vec->size)
    {
        fprintf(stderr, "Index out of bounds\n");
        exit(1);
    }
    return vec->data[index];
}

// 设置向量元素
void vector_set(MyVector *vec, size_t index, int value)
{
    if (index >= vec->size)
    {
        fprintf(stderr, "Index out of bounds\n");
        exit(1);
    }
    vec->data[index] = value;
}

// 清理内存
void vector_free(MyVector *vec)
{
    free(vec->data);
}
