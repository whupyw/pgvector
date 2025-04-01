#ifndef SCRATCH_H
#define SCRATCH_H

#include "my_vector.h" // 引入 NewVector.h
#include "bit_array.h" // 如果 bit_array 相关代码被拆分，可以引入相应头文件
#include "neighbour.h" // 引入邻居优先队列相关头文件
#include "new_vector.h"

// 定义一个结构体，包含三个指针
typedef struct
{
    NewVector *expanded_nodes;           // 已经扩展的节点
    uint32_t *is_visited;                // 已经访问的节点
    NeighborPriorityQueue *best_L_nodes; // 指向 NeighborPriorityQueue
    size_t *entry_point;
    size_t *max_point;
    uint32_t cur_node;
} Scratch;

// 初始化 Scratch 结构体的方法
void init_scratch(Scratch *scratch, size_t vector_capacity, size_t L_Size, size_t entry_point, uint32_t cur_node)
{
    // 初始化 NewVector
    scratch->expanded_nodes = (NewVector *)malloc(sizeof(NewVector));
    new_vector_init(scratch->expanded_nodes, sizeof(Neighbor)); // 使用 NewVector 的初始化函数

    // 初始化 bit_array
    scratch->is_visited = (uint32_t *)calloc(vector_capacity, sizeof(uint32_t)); // 使用 calloc 确保初始化为0

    // 初始化 NeighborPriorityQueue
    scratch->best_L_nodes = (NeighborPriorityQueue *)malloc(sizeof(NeighborPriorityQueue));
    init_queue(scratch->best_L_nodes, L_Size); // 使用 NeighborPriorityQueue 的初始化函数

    scratch->entry_point = (size_t *)malloc(sizeof(size_t));
    scratch->entry_point = entry_point;
    scratch->max_point = (size_t *)malloc(sizeof(size_t));
    scratch->max_point = vector_capacity;

    scratch->cur_node = cur_node;
}

// 清理 Scratch 结构体占用的内存
void free_scratch(Scratch *scratch)
{
    new_vector_free(scratch->expanded_nodes); // 使用 NewVector 的释放函数
    free_queue(scratch->best_L_nodes);        // 使用 NeighborPriorityQueue 的释放函数
    free(scratch->expanded_nodes);            // 释放 NewVector 内存
    free(scratch->is_visited);                // 释放 bit_array 内存
    free(scratch->best_L_nodes);              // 释放结构体内存
}

#endif // SCRATCH_H
