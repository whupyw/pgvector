#include "postgres.h"
#include "executor/spi.h"
#include "store_index.h"
#include <stdio.h>
#include <stdlib.h>
#include "my_vector.h"

extern const NewVector *static_neighbors_vectors;

bool create_disk_laylout()
{
    elog(INFO, "create disk layout");
    bool ret = save_neighbors_to_disk(static_neighbors_vectors);
    return ret;
}

bool save_neighbors_to_disk(NewVector *neighbors)
{
    // 连接到 PostgreSQL 内部 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(ERROR, "SPI_connect failed");
        return false;
    }

    // 获取数据表中记录的数量（例如：3 行数据）
    const char *select_query = "SELECT id FROM vectors"; // 假设我们根据 `id` 来更新
    int ret = SPI_exec(select_query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "SPI_exec failed: %s", select_query);
        SPI_finish();
        return false;
    }

    size_t npts = (size_t)SPI_processed;
    elog(INFO, "Number of records: %zu", npts);
    if (npts == 0)
    {
        elog(WARNING, "No records found in table vectors.");
        SPI_finish();
        return false;
    }
    // 记录所有id 1->numpoint
    MyVector *ids = (MyVector *)palloc(sizeof(MyVector));
    vector_init(ids);
    for (size_t i = 0; i < npts; i++)
    {
        bool isnull = false;
        HeapTuple tuple = SPI_tuptable->vals[i];
        Datum val = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (isnull)
            continue;
        uint32_t id = DatumGetUInt32(val);
        // 记录id
        vector_push_back(ids, id);
    }

    // 遍历每一条记录，根据 `id` 更新邻居
    for (size_t i = 0; i < npts; i++)
    {
        // 获取当前记录的 `id` id = 1 i = 0
        uint32_t id = vector_get(ids, i);
        // 假设二维数组 `neighbors_array` 中每行数据对应于一个记录的邻居
        // 构建邻居数组字符串
        char array_string[100];
        array_string[0] = '\0'; // 初始化空字符串，确保之前的内容被清空
        // snprintf(array_string, sizeof(array_string), "%d,%d,%d", neighbors_array[i][0], neighbors_array[i][1], neighbors_array[i][2]);
        NewVector *cur_node_neighbors = new_vector_get(neighbors, i);
        for (size_t j = 0; j < cur_node_neighbors->size; j++)
        {
            if (j > 0)
            {
                strcat(array_string, ",");
            }
            uint32_t *cur_node_neighbor = new_vector_get(cur_node_neighbors, j);
            char neighbor_string[32]; // 假设 uint32_t 足够小，不会超过 32 个字符
            snprintf(neighbor_string, sizeof(neighbor_string), "%u", *cur_node_neighbor);
            strcat(array_string, neighbor_string);
        }
        char query[256];
        snprintf(query, sizeof(query), "UPDATE vectors SET neighbors = ARRAY[%s] WHERE id = %d", array_string, id);
        elog(INFO, "Executing query: %s", query);

        // 执行更新操作
        ret = SPI_exec(query, 1); // 更新 1 行
        if (ret != SPI_OK_UPDATE)
        {
            elog(ERROR, "Failed to update neighbor for id %d", id);
            SPI_finish();
            return false;
        }
    }

    // 关闭 SPI 连接
    SPI_finish();
    vector_free(ids);
    pfree(ids);
    return true;
}