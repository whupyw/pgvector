#include "postgres.h"
#include "kv_table.h"
#include <stdio.h>
#include <stdlib.h>

unsigned int kv_hash(uint32_t key)
{
    return key % TABLE_SIZE;
}

kv_table *create_kv_table()
{
    kv_table *new_table = (kv_table *)palloc(sizeof(kv_table));
    if (new_table == NULL)
    {
        printf("Memory allocation failed\n");
        exit(1);
    }
    for (int i = 0; i < TABLE_SIZE; i++)
    {
        new_table->table[i] = NULL;
    }
    return new_table;
}

int kv_insert(kv_table *table, uint32_t key, uint32_t value)
{
    unsigned int index = kv_hash(key);

    // 如果当前位置已经有数据，则插入失败
    if (table->table[index] != NULL)
    {
        return 0; // 返回 0 表示插入失败
    }

    // 插入新的键值对
    kv_pair *new_pair = (kv_pair *)palloc(sizeof(kv_pair));
    if (new_pair == NULL)
    {
        printf("Memory allocation failed\n");
        return 0; // 内存分配失败时返回插入失败
    }
    new_pair->key = key;
    new_pair->value = value;
    table->table[index] = new_pair;

    return 1; // 返回 1 表示插入成功
}

uint32_t kv_get(kv_table *table, uint32_t key)
{
    unsigned int index = kv_hash(key);

    // 如果当前位置有数据且 key 匹配，则返回对应的值
    if (table->table[index] != NULL && table->table[index]->key == key)
    {
        return table->table[index]->value;
    }

    return -1; // 如果没有找到键，返回 -1 表示未找到
}

void kv_delete(kv_table *table, uint32_t key)
{
    unsigned int index = kv_hash(key);

    // 如果当前位置有数据且 key 匹配，则删除该键值对
    if (table->table[index] != NULL && table->table[index]->key == key)
    {
        free(table->table[index]);
        table->table[index] = NULL;
    }
}

void destroy_kv_table(kv_table *table)
{
    for (int i = 0; i < TABLE_SIZE; i++)
    {
        if (table->table[i] != NULL)
        {
            pfree(table->table[i]);
        }
    }
    pfree(table);
}
