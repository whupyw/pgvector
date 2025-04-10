#ifndef KV_TABLE_H
#define KV_TABLE_H

#include <stdint.h>

#define TABLE_SIZE 1000

// 键值对结构体
typedef struct kv_pair
{
    uint32_t key;
    uint32_t value;
} kv_pair;

// 哈希表结构体
typedef struct kv_table
{
    kv_pair *table[TABLE_SIZE];
} kv_table;

// 函数声明

// 初始化 KV 容器
kv_table *create_kv_table();

// 插入键值对，如果发生冲突则插入失败
int kv_insert(kv_table *table, uint32_t key, uint32_t value);

// 查找值
uint32_t kv_get(kv_table *table, uint32_t key);

// 删除键值对
void kv_delete(kv_table *table, uint32_t key);

// 销毁 KV 容器，释放内存
void destroy_kv_table(kv_table *table);

#endif // KV_TABLE_H
