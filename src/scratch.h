#ifndef SCRATCH_H
#define SCRATCH_H

#include "my_vector.h"  // 引入 MyVector.h
#include "bit_array.h" // 如果 bit_array 相关代码被拆分，可以引入相应头文件
#include "neighbour.h" // 引入邻居优先队列相关头文件

// 定义一个结构体，包含三个指针
typedef struct
{
    MyVector *vector_ptr;             // 已经扩展的节点
    uint32_t *bit_array_ptr;          // 已经访问的节点
    NeighborPriorityQueue *queue_ptr; // 指向 NeighborPriorityQueue
} Scratch;

// 初始化 Scratch 结构体的方法
void init_scratch(Scratch *scratch, size_t vector_capacity, size_t bit_array_size, size_t L_Size)
{
    // 初始化 MyVector
    //scratch->vector_ptr = (MyVector *)malloc(sizeof(MyVector));
    vector_init(scratch->vector_ptr); // 使用 MyVector 的初始化函数

    // 初始化 bit_array
    scratch->bit_array_ptr = (uint32_t *)calloc(bit_array_size, sizeof(uint32_t)); // 使用 calloc 确保初始化为0

    // 初始化 NeighborPriorityQueue
    //scratch->queue_ptr = (NeighborPriorityQueue *)malloc(sizeof(NeighborPriorityQueue));
    init_queue(scratch->queue_ptr, L_Size); // 使用 NeighborPriorityQueue 的初始化函数
}

// 清理 Scratch 结构体占用的内存
void free_scratch(Scratch *scratch)
{
    free(scratch->vector_ptr);      // 释放 MyVector 内存
    free(scratch->bit_array_ptr);   // 释放 bit_array 内存
    free_queue(scratch->queue_ptr); // 释放 NeighborPriorityQueue 内存
    free(scratch->queue_ptr);       // 释放结构体内存
}

#endif // SCRATCH_H
