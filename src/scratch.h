#ifndef SCRATCH_H
#define SCRATCH_H
#include "postgres.h"  // 引入 Postgres 头文件
#include "my_vector.h" // 引入 NewVector.h
#include "bit_array.h" // 如果 bit_array 相关代码被拆分，可以引入相应头文件
#include "neighbour.h" // 引入邻居优先队列相关头文件
#include "new_vector.h"

// 定义一个结构体，包含三个指针
typedef struct
{
    size_t entry_point;
    size_t max_point;
    uint32_t cur_node;
    NewVector *expanded_nodes; // 已经扩展的节点
    // uint32_t *is_visited;                // 已经访问的节点
    NeighborPriorityQueue *best_L_nodes; // 指向 NeighborPriorityQueue
    NewVector *neighbors;
} Scratch;

// 初始化 Scratch 结构体的方法
void init_scratch(Scratch *scratch, size_t vector_capacity, size_t L_Size, size_t entry_point, uint32_t cur_node)
{
    // 初始化 NewVector
    scratch->expanded_nodes = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(scratch->expanded_nodes, sizeof(Neighbor), 70); // 使用 NewVector 的初始化函数

    // 初始化 bit_array
    // scratch->is_visited = (uint32_t *)palloc0(vector_capacity); // 使用 palloc0 确保初始化为0

    // 初始化 NeighborPriorityQueue
    scratch->best_L_nodes = (NeighborPriorityQueue *)palloc(sizeof(NeighborPriorityQueue));
    size_t new_L_size = 3 * L_Size;
    init_queue(scratch->best_L_nodes, new_L_size); // 使用 NeighborPriorityQueue 的初始化函数

    scratch->entry_point = entry_point;
    scratch->max_point = vector_capacity;

    scratch->cur_node = cur_node;
    scratch->neighbors = NULL;
}

// 清理 Scratch 结构体占用的内存
void free_scratch(Scratch *scratch)
{
    new_vector_free(scratch->expanded_nodes); // 使用 NewVector 的释放函数
    free_queue(scratch->best_L_nodes);        // 使用 NeighborPriorityQueue 的释放函数
    if (scratch != NULL && scratch->expanded_nodes)
    {
        pfree(scratch->expanded_nodes); // 释放 NewVector 内存
        scratch->expanded_nodes = NULL; // 防止悬挂指针
    }
    // pfree(scratch->is_visited);   // 释放 bit_array 内存
    pfree(scratch->best_L_nodes); // 释放结构体内存
}

#endif // SCRATCH_H
