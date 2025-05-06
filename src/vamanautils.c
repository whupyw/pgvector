#include "postgres.h"

#include <math.h>
#include "executor/spi.h"
#include "access/generic_xlog.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "lib/pairingheap.h"
#include "sparsevec.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/memdebug.h"
#include "utils/rel.h"
#include "vamana.h"
#include "new_vector.h"
#include <utils/array.h>
#include "vamana_index.h"
#include "bit_array.h"
#include "kv_table.h"
#include "lib/stringinfo.h"

#define FULL_PRECISION_REORDER_MULTIPLIER 10

/* 初始化Vamana索引 */
void vamanainit(void)
{
    // // 注册自定义参数
    // DefineCustomIntVariable("vamana.r",
    //                         "Sets the candidate set size for Vamana index",
    //                         NULL,
    //                         &vamana_r,
    //                         VAMANA_DEFAULT_R,
    //                         1, 1000,
    //                         PGC_USERSET,
    //                         0,
    //                         NULL, NULL, NULL);

    // DefineCustomIntVariable("vamana.l",
    //                         "Sets the search list size for Vamana index",
    //                         NULL,
    //                         &vamana_l,
    //                         VAMANA_DEFAULT_L,
    //                         1, 1000,
    //                         PGC_USERSET,
    //                         0,
    //                         NULL, NULL, NULL);

    // DefineCustomRealVariable("vamana.alpha",
    //                          "Sets the alpha parameter for Vamana index",
    //                          NULL,
    //                          &vamana_alpha,
    //                          VAMANA_DEFAULT_ALPHA,
    //                          0.1, 10.0,
    //                          PGC_USERSET,
    //                          0,
    //                          NULL, NULL, NULL);

    // // 初始化锁
    // RequestAddinShmemSpace(MAXALIGN(sizeof(int)));
    // RequestNamedLWLockTranche("vamana", 1);
}

/*
 * New buffer
 */
Buffer
VamanaNewBuffer(Relation index, ForkNumber forkNum)
{
    Buffer buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);

    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

/*
 * Init page
 */
void VamanaInitPage(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(VamanaPageOpaqueData));
    VamanaPageGetOpaque(page)->nextblkno = InvalidBlockNumber;
    VamanaPageGetOpaque(page)->page_id = VAMANA_PAGE_ID;
}

bool create_index_table(char *index_name, char *table_name, int dimensions)
{
    const char *source_table = table_name;
    const char *target_table = index_name;

    // 创建目标索引表
    /* 创建表（如果不存在） */
    // 启动 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    // 检查vector
    if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pgvector extension not installed")));
    }
    if (SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("vector type not found")));
    }

    SPI_execute("SET search_path TO public, pg_catalog;", false, 0);

    // 检查表是否存在
    char create_table_sql[512];
    snprintf(create_table_sql, sizeof(create_table_sql),
             "CREATE TABLE IF NOT EXISTS %s ("
             "id SERIAL PRIMARY KEY,vector_id INTEGER UNIQUE,"
             "embedding vector(%d),neighbors integer[]);",
             target_table, dimensions);
    elog(INFO, "create table sql: %s", create_table_sql);

    if (SPI_execute(create_table_sql, false, 0) != SPI_OK_UTILITY)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("Table creation failed: %s", SPI_result_code_string(SPI_result))));
        return false;
    }
    char insert_sql[512];
    snprintf(insert_sql, sizeof(insert_sql),
             "INSERT INTO %s (vector_id, embedding) "
             "SELECT id - 1 AS vector_id,embedding FROM %s;",
             target_table, source_table);
    elog(INFO, "insert sql: %s", insert_sql);
    // 执行 SQL
    if (SPI_execute(insert_sql, false, 0) != SPI_OK_INSERT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("Table insertion failed: %s", SPI_result_code_string(SPI_result))));
        return false;
    }
    // elog(INFO, "Inserted %lu rows into %s", SPI_processed, target_table);
    SPI_finish();
    return true;
}

bool get_vector_in_database(char *index_table_name, uint32_t vector_id, Vector *vec)
{
    // 启动 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    // 检查vector
    if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pgvector extension not installed")));
    }

    if (SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("vector type not found")));
    }

    SPI_execute("SET search_path TO public, pg_catalog;", false, 0);

    // 进行查询
    // 构建查询语句
    // char query[256];
    // snprintf(query, sizeof(query),
    //          "SELECT vector_id, embedding, neighbors FROM %s ORDER BY vector_id", index_table_name);

    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT vector_id, embedding FROM %s WHERE vector_id =%d", index_table_name, vector_id);

    if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        elog(ERROR, "Failed to execute query: %s", query);
    }

    // 获取结果
    uint64_t num_rows = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    HeapTuple tuple = tuptable->vals[0];

    bool isnull = false;

    // embedding (vector)
    Datum d_embedding = SPI_getbinval(tuple, tupdesc, 2, &isnull);
    if (isnull)
        return false;

    Vector *vec_data = (Vector *)DatumGetVector(d_embedding); // pgvector 内部结构返回 float*
    if (vec_data->dim != vec->dim)
    {
        elog(ERROR, "vector dim not match");
        return false;
    }
    for (int i = 0; i < vec_data->dim; i++)
    {
        vec->x[i] = vec_data->x[i];
    }
    SPI_finish();
    return true;
}

void get_vectors_and_neighbors(char *index_table_name, NewVector *re_vectors, NewVector *target_vectors)
{
    // 启动 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    // 检查vector
    if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pgvector extension not installed")));
    }

    if (SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("vector type not found")));
    }

    SPI_execute("SET search_path TO public, pg_catalog;", false, 0);

    // 进行查询
    // 构建查询语句
    // char query[256];
    // snprintf(query, sizeof(query),
    //          "SELECT vector_id, embedding, neighbors FROM %s ORDER BY vector_id", index_table_name);

    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT vector_id, embedding, neighbors FROM %s WHERE vector_id IN (", index_table_name);

    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        uint32_t vector_id = *vector_id_pointer;
        if (i > 0)
        {
            snprintf(query + strlen(query), sizeof(query) - strlen(query), ",%d", vector_id);
        }
        else
        {
            snprintf(query + strlen(query), sizeof(query) - strlen(query), "%d", vector_id);
        }
    }
    // snprintf(query + strlen(query), sizeof(query) - strlen(query), ")");
    snprintf(query + strlen(query), sizeof(query) - strlen(query), ") ORDER BY CASE vector_id ");
    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        uint32_t vector_id = *vector_id_pointer;
        snprintf(query + strlen(query), sizeof(query) - strlen(query),
                 "WHEN %d THEN %zu ", vector_id, i + 1);
    }
    snprintf(query + strlen(query), sizeof(query) - strlen(query), "END");
    // elog(INFO, "query: %s", query);
    if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        elog(ERROR, "Failed to execute query: %s", query);
    }

    // 获取结果
    uint64_t num_rows = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    for (uint64_t i = 0; i < num_rows; i++)
    {
        HeapTuple tuple = tuptable->vals[i];

        bool isnull = false;

        // vector_id
        Datum d_vector_id = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (isnull)
            continue;

        uint32_t vector_id = DatumGetUInt32(d_vector_id);

        // embedding (vector)
        Datum d_embedding = SPI_getbinval(tuple, tupdesc, 2, &isnull);
        if (isnull)
            continue;

        Vector *vec_data = (Vector *)DatumGetVector(d_embedding); // pgvector 内部结构返回 float*

        // neighbors (integer[])
        Datum d_neighbors = SPI_getbinval(tuple, tupdesc, 3, &isnull);
        ArrayType *arr = NULL;
        uint32_t *neighbors = NULL;
        int n_neighbors = 0;

        if (!isnull)
        {
            arr = DatumGetArrayTypeP(d_neighbors);
            if (ARR_NDIM(arr) != 1)
                elog(ERROR, "neighbors must be a 1D array");

            n_neighbors = (int)ARR_DIMS(arr)[0];
            neighbors = (uint32_t *)palloc(sizeof(uint32_t) * n_neighbors);

            int16 elmlen;
            bool elmbyval;
            char elmalign;
            get_typlenbyvalalign(INT4OID, &elmlen, &elmbyval, &elmalign);

            Datum *elems;
            bool *nulls;
            int nelems;
            deconstruct_array(arr, INT4OID, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);

            for (int j = 0; j < nelems; j++)
            {
                neighbors[j] = DatumGetUInt32(elems[j]);
            }

            pfree(elems);
            pfree(nulls);
        }

        // 构造 VectorCache
        VectorCache item;
        item.vector_id = vector_id;
        item.vector = vec_data;
        item.neighbor_count = n_neighbors;
        item.neighbors = neighbors;

        // 加入 re_vectors 容器
        new_vector_push_back(re_vectors, &item);
    }
    SPI_finish();
}

void new_get_vectors(char *index_table_name, NewVector *re_vectors, NewVector *target_vectors)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pgvector extension not installed")));
    }

    if (SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("vector type not found")));
    }

    SPI_execute("SET search_path TO public, pg_catalog;", false, 0);

    // 使用 StringInfo 动态构建 SQL 查询语句
    StringInfoData buf;
    initStringInfo(&buf);

    appendStringInfo(&buf, "SELECT vector_id, embedding FROM %s WHERE vector_id IN (", index_table_name);

    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        appendStringInfo(&buf, "%s%u", i > 0 ? "," : "", *vector_id_pointer);
    }

    appendStringInfoString(&buf, ") ORDER BY CASE vector_id ");

    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        appendStringInfo(&buf, "WHEN %u THEN %zu ", *vector_id_pointer, i + 1);
    }

    appendStringInfoString(&buf, "END");

    // 执行拼接好的 SQL
    if (SPI_execute(buf.data, true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        elog(ERROR, "Failed to execute query: %s", buf.data);
    }

    // 处理返回结果，只获取 vector_id 和 embedding
    uint64_t num_rows = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    for (uint64_t i = 0; i < num_rows; i++)
    {
        HeapTuple tuple = tuptable->vals[i];
        bool isnull = false;

        Datum d_vector_id = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (isnull)
            continue;

        uint32_t vector_id = DatumGetUInt32(d_vector_id);

        Datum d_embedding = SPI_getbinval(tuple, tupdesc, 2, &isnull);
        if (isnull)
            continue;

        Vector *vec_data = (Vector *)DatumGetVector(d_embedding);

        VectorCache item;
        item.vector_id = vector_id;
        item.vector = vec_data;
        item.neighbor_count = 0;
        item.neighbors = NULL;

        new_vector_push_back(re_vectors, &item);
    }

    SPI_finish();
}

void new_get_vectors_and_neighbors(char *index_table_name, NewVector *re_vectors, NewVector *target_vectors)
{
    // elog(INFO, "new get_vectors_and_neighbors");
    //  启动 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
        elog(ERROR, "SPI_connect failed");

    // 检查vector
    if (SPI_execute("SELECT 1 FROM pg_type WHERE typname = 'vector'", true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("pgvector extension not installed")));
    }

    if (SPI_processed == 0)
    {
        SPI_finish();
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("vector type not found")));
    }

    SPI_execute("SET search_path TO public, pg_catalog;", false, 0);

    // 进行查询
    // 构建查询语句
    // char query[256];
    // snprintf(query, sizeof(query),
    //          "SELECT vector_id, embedding, neighbors FROM %s ORDER BY vector_id", index_table_name);

    StringInfoData buf;
    initStringInfo(&buf);

    appendStringInfo(&buf, "SELECT vector_id, embedding, neighbors FROM %s WHERE vector_id IN (", index_table_name);

    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        if (i > 0)
            appendStringInfo(&buf, ",%u", *vector_id_pointer);
        else
            appendStringInfo(&buf, "%u", *vector_id_pointer);
    }
    appendStringInfoString(&buf, ") ORDER BY CASE vector_id ");
    for (size_t i = 0; i < target_vectors->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(target_vectors, i);
        appendStringInfo(&buf, "WHEN %u THEN %zu ", *vector_id_pointer, i + 1);
    }
    appendStringInfoString(&buf, "END");
    // elog(INFO, "query: %s", query);
    if (SPI_execute(buf.data, true, 0) != SPI_OK_SELECT)
    {
        SPI_finish();
        elog(ERROR, "Failed to execute query: %s", buf.data);
    }

    // 获取结果
    uint64_t num_rows = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;
    for (uint64_t i = 0; i < num_rows; i++)
    {
        HeapTuple tuple = tuptable->vals[i];

        bool isnull = false;

        // vector_id
        Datum d_vector_id = SPI_getbinval(tuple, tupdesc, 1, &isnull);
        if (isnull)
            continue;

        uint32_t vector_id = DatumGetUInt32(d_vector_id);

        // embedding (vector)
        Datum d_embedding = SPI_getbinval(tuple, tupdesc, 2, &isnull);
        if (isnull)
            continue;

        Vector *vec_data = (Vector *)DatumGetVector(d_embedding); // pgvector 内部结构返回 float*

        // neighbors (integer[])
        Datum d_neighbors = SPI_getbinval(tuple, tupdesc, 3, &isnull);
        ArrayType *arr = NULL;
        uint32_t *neighbors = NULL;
        int n_neighbors = 0;

        if (!isnull)
        {
            arr = DatumGetArrayTypeP(d_neighbors);
            if (ARR_NDIM(arr) != 1)
                elog(ERROR, "neighbors must be a 1D array");

            n_neighbors = (int)ARR_DIMS(arr)[0];
            neighbors = (uint32_t *)palloc(sizeof(uint32_t) * n_neighbors);

            int16 elmlen;
            bool elmbyval;
            char elmalign;
            get_typlenbyvalalign(INT4OID, &elmlen, &elmbyval, &elmalign);

            Datum *elems;
            bool *nulls;
            int nelems;
            deconstruct_array(arr, INT4OID, elmlen, elmbyval, elmalign, &elems, &nulls, &nelems);

            for (int j = 0; j < nelems; j++)
            {
                neighbors[j] = DatumGetUInt32(elems[j]);
            }

            pfree(elems);
            pfree(nulls);
        }

        // 构造 VectorCache
        VectorCache item;
        item.vector_id = vector_id;
        item.vector = vec_data;
        item.neighbor_count = n_neighbors;
        item.neighbors = neighbors;

        // 加入 re_vectors 容器
        new_vector_push_back(re_vectors, &item);
    }
    SPI_finish();
}

NewVector *search_k_nearest_neighbors(char *index_table_name, uint32_t init_id,
                                      int k, Vector *target, uint32_t vector_num)
{
    // 要考虑的点，邻居肯定不是全加载
    // （可选） 预先加载三跳以内的向量和邻居

    // 加载PQ查表和压缩向量
    // 选择起点 就是建索引的点

    // 初始化数据结构
    // visited = new bool[vector_num];
    // 优先队列
    // 一个存储Neighbor的全部候选节点

    uint32_t io_limit = 100000;
    uint32_t num_ios = 0;
    uint32_t beam_width = 64;

    NeighborPriorityQueue *retset;
    uint32_t *is_visited;
    NewVector *full_retset;
    NewVector *frontier;
    // 存储准备查询的节点
    NewVector *frontier_nhoods;
    NewVector *frontier_nhoods_req;
    // NewVector *vector_caches;
    //  缓存机制
    size_t total_visit_size = 0;
    NewVector *res_vector_ids;

    retset = (NeighborPriorityQueue *)palloc(sizeof(NeighborPriorityQueue));
    init_queue(retset, k);
    is_visited = (uint32_t *)calloc(vector_num / 32 + 1, sizeof(uint32_t));
    full_retset = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init(full_retset, sizeof(Neighbor));
    frontier = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods_req = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods_req, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods, sizeof(VectorCache), 2 * beam_width);

    res_vector_ids = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(res_vector_ids, sizeof(uint32_t), k + 1);
    // vector_caches = (NewVector *)palloc(sizeof(NewVector));
    // new_vector_init_with_capacity(vector_caches, sizeof(VectorCache), 3 * beam_width);

    // 获取初始节点的id和dist 加入候选集
    Vector *init_vec = InitVector(target->dim);
    get_vector_in_database("vectors_index_table", init_id, init_vec);
    float dist = vector_L2_distance(target->dim, target->x, init_vec->x);
    Neighbor init_node;
    init_node.id = init_id;
    init_node.distance = dist;
    priority_queue_insert(retset, init_node);
    set_bit(is_visited, (size_t)init_id);

    // 先不用缓存
    while (has_unexpanded_node(retset) && num_ios < io_limit)
    {
        // new_beam
        uint32_t num_seen = 0;
        new_vector_clear(frontier);
        // frontier->size = 0;
        new_vector_clear(frontier_nhoods_req);
        new_vector_clear(frontier_nhoods);
        while (has_unexpanded_node(retset) && frontier->size < beam_width && num_seen < beam_width)
        {
            Neighbor nbr = closest_unexpanded(retset);
            num_seen++;
            uint32_t temp_id = nbr.id;
            // 判断该节点的邻居是否已经被缓存
            // 是 直接使用缓存的邻居数据
            // 否 加入frontier 准备缓存
            if (false)
            {
            }
            else
            {
                // frontier.push_back(nbr.id);
                new_vector_push_back(frontier, &temp_id);
                // elog(INFO, "frontier: %u", temp_id);
            }
        }

        if (frontier->size != 0)
        {
            for (uint32_t i = 0; i < frontier->size; i++)
            {
                // 准备加载frontier中的节点的邻居
                uint32_t *current_id_pointer = new_vector_get(frontier, i);
                // DEBUG 这里会不会有内存分配的问题
                new_vector_push_back(frontier_nhoods_req, current_id_pointer);
                num_ios++;
            }
        }

        // 加载frontier中的节点的邻居
        // select vector and neighbors from disk
        new_get_vectors_and_neighbors(index_table_name, frontier_nhoods, frontier_nhoods_req);
        // elog(INFO, "frontier_nhoods_req: %u", frontier_nhoods_req->size);
        total_visit_size += frontier_nhoods_req->size;
        new_vector_clear(frontier_nhoods_req);
        // new_vector_append(vector_caches, frontier_nhoods);

        // 处理缓存中的邻居
        // 拿出一个节点
        // 计算与查询向量的PQ距离
        // 将当前节点加入full_retset
        // 拿出这个节点的邻居
        // 计算与查询向量的PQ距离
        // 加入best_l_node

        // 循环frontier_nhoods
        // 获取邻居数量和坐标
        // 计算当前节点与查询向量的距离
        // 处理邻居节点
        for (uint32_t i = 0; i < frontier_nhoods->size; i++)
        {
            VectorCache *item = (VectorCache *)new_vector_get(frontier_nhoods, i);
            // uint32_t id = item->vector_id;
            uint32_t n_count = item->neighbor_count;
            Vector *item_vec = item->vector;
            float tmp_distance = vector_L2_distance(target->dim, target->x, item_vec->x);
            Neighbor nbr;
            nbr.id = item->vector_id;
            nbr.distance = tmp_distance;
            new_vector_push_back(full_retset, &nbr);

            // 检查邻居
            for (uint32_t m = 0; m < n_count; m++)
            {
                uint32_t id = item->neighbors[m];
                if (!test_bit(is_visited, (size_t)id))
                {
                    // 如果没访问过
                    // 计算距离 应该是计算与查询向量的距离
                    float distance = get_distance_to_target_by_id(id, target, target->dim);
                    Neighbor nn;
                    nn.id = id;
                    nn.distance = distance;
                    nn.expanded = false;
                    set_bit(is_visited, (size_t)id);
                    // 将邻居加入候选集
                    priority_queue_insert(retset, nn);
                }
            }
        }
        new_vector_clear(frontier_nhoods);
    }
    // 排序full_retset
    bool use_full_sort = true;
    if (use_full_sort)
    {
        // 隐式重排序
        // 将full_retset的距离替换为真实距离

        // 截断一部分节点
        new_vector_sort(full_retset, compare_neighbors);
        if (full_retset->size > k * FULL_PRECISION_REORDER_MULTIPLIER)
        {
            new_vector_resize(full_retset, k * FULL_PRECISION_REORDER_MULTIPLIER);
        }

        // kv_table *my_kv_table = create_kv_table();
        new_vector_clear(frontier_nhoods);
        new_vector_clear(frontier_nhoods_req);

        for (size_t i = 0; i < full_retset->size; i++)
        {
            // 判断哪些在内存哪些不在
            // 先全部从磁盘中获取
            uint32_t n_id = ((Neighbor *)new_vector_get(full_retset, i))->id;
            // elog(INFO, "isert_id: %d", n_id);
            //  int ret = kv_insert(my_kv_table, n_id, i);
            //  if (ret == 0)
            //  {
            //      elog(ERROR, "kv_insert failed");
            //  }
            new_vector_push_back(frontier_nhoods_req, &n_id);
        }

        new_get_vectors(index_table_name, frontier_nhoods, frontier_nhoods_req);

        if (frontier_nhoods->size != full_retset->size)
        {
            elog(ERROR, "frontier_nhoods->size != full_retset->size");
        }

        for (size_t i = 0; i < full_retset->size; i++)
        {
            VectorCache *cur = new_vector_get(frontier_nhoods, i);
            // elog(INFO, "isert_id: %d", cur->vector_id);
            float true_dist = vector_L2_distance(target->dim, target->x, cur->vector->x);
            Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
            nbr->distance = true_dist;
        }
    }

    new_vector_sort(full_retset, compare_neighbors);

    for (uint32_t i = 0; i < k && i < full_retset->size; i++)
    {
        Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
        // elog(INFO, "id: %d, distance: %f", nbr->id, nbr->distance);
        uint32_t nbr_id = nbr->id;
        float dist = nbr->distance;
        elog(INFO, "id: %d, distance: %f", nbr_id, dist);
        new_vector_push_back(res_vector_ids, &nbr_id);
    }

    free(is_visited);
    new_vector_free(frontier);
    new_vector_free(frontier_nhoods);
    new_vector_free(frontier_nhoods_req);
    new_vector_free(full_retset);
    // new_vector_free(vector_caches);
    // pfree(vector_caches);
    free_queue(retset);
    pfree(frontier);
    pfree(frontier_nhoods);
    pfree(frontier_nhoods_req);
    pfree(retset);
    pfree(full_retset);

    return res_vector_ids;
}

NewVector *new_search_k_nearest_neighbors(char *index_table_name, NewVector *init_ids,
                                          int k, Vector *target, uint32_t vector_num)
{
    // 要考虑的点，邻居肯定不是全加载
    // （可选） 预先加载三跳以内的向量和邻居

    // 加载PQ查表和压缩向量
    // 选择起点 就是建索引的点

    // 初始化数据结构
    // visited = new bool[vector_num];
    // 优先队列
    // 一个存储Neighbor的全部候选节点
    size_t total_visit_size = 0;

    uint32_t io_limit = 100000;
    uint32_t num_ios = 0;
    uint32_t beam_width = 35;

    NeighborPriorityQueue *retset;
    uint32_t *is_visited;
    NewVector *full_retset;
    NewVector *frontier;
    // 存储准备查询的节点
    NewVector *frontier_nhoods;
    NewVector *frontier_nhoods_req;
    NewVector *vector_caches;
    //  缓存机制

    NewVector *res_vector_ids;

    uint32_t retset_size = 90;

    retset = (NeighborPriorityQueue *)palloc(sizeof(NeighborPriorityQueue));
    init_queue(retset, retset_size);
    is_visited = (uint32_t *)calloc(vector_num / 32 + 1, sizeof(uint32_t));
    full_retset = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init(full_retset, sizeof(Neighbor));
    frontier = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods_req = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods_req, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods, sizeof(VectorCache), 2 * beam_width);

    res_vector_ids = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(res_vector_ids, sizeof(uint32_t), k + 1);
    vector_caches = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(vector_caches, sizeof(VectorCache), 3 * beam_width);

    // 获取初始节点的id和dist 加入候选集
    for (size_t i = 0; i < init_ids->size; i++)
    {
        uint32_t *init_id_pointer = new_vector_get(init_ids, i);
        uint32_t init_id = *init_id_pointer;
        Vector *init_vec = InitVector(target->dim);
        get_vector_in_database("vectors_index_table", init_id, init_vec);
        float dist = vector_L2_distance(target->dim, target->x, init_vec->x);
        Neighbor init_node;
        init_node.id = init_id;
        init_node.distance = dist;
        priority_queue_insert(retset, init_node);
        set_bit(is_visited, (size_t)init_id);
    }

    // 先不用缓存
    while (has_unexpanded_node(retset) && num_ios < io_limit)
    {
        // new_beam
        uint32_t num_seen = 0;
        new_vector_clear(frontier);
        // frontier->size = 0;
        new_vector_clear(frontier_nhoods_req);
        new_vector_clear(frontier_nhoods);
        while (has_unexpanded_node(retset) && frontier->size < beam_width && num_seen < beam_width)
        {
            Neighbor nbr = closest_unexpanded(retset);
            num_seen++;
            uint32_t temp_id = nbr.id;
            // 判断该节点的邻居是否已经被缓存
            // 是 直接使用缓存的邻居数据
            // 否 加入frontier 准备缓存
            if (false)
            {
            }
            else
            {
                // frontier.push_back(nbr.id);
                new_vector_push_back(frontier, &temp_id);
                // elog(INFO, "frontier: %u", temp_id);
            }
        }

        if (frontier->size != 0)
        {
            for (uint32_t i = 0; i < frontier->size; i++)
            {
                // 准备加载frontier中的节点的邻居
                uint32_t *current_id_pointer = new_vector_get(frontier, i);
                // DEBUG 这里会不会有内存分配的问题
                new_vector_push_back(frontier_nhoods_req, current_id_pointer);
                num_ios++;
            }
        }

        // 加载frontier中的节点的邻居
        // select vector and neighbors from disk
        new_get_vectors_and_neighbors(index_table_name, frontier_nhoods, frontier_nhoods_req);
        new_vector_append(vector_caches, frontier_nhoods);
        total_visit_size += frontier_nhoods_req->size;
        new_vector_clear(frontier_nhoods_req);

        // 处理缓存中的邻居
        // 拿出一个节点
        // 计算与查询向量的PQ距离
        // 将当前节点加入full_retset
        // 拿出这个节点的邻居
        // 计算与查询向量的PQ距离
        // 加入best_l_node

        // 循环frontier_nhoods
        // 获取邻居数量和坐标
        // 计算当前节点与查询向量的距离
        // 处理邻居节点
        for (uint32_t i = 0; i < frontier_nhoods->size; i++)
        {
            VectorCache *item = (VectorCache *)new_vector_get(frontier_nhoods, i);
            // uint32_t id = item->vector_id;
            uint32_t n_count = item->neighbor_count;
            Vector *item_vec = item->vector;
            float tmp_distance = vector_L2_distance(target->dim, target->x, item_vec->x);
            Neighbor nbr;
            nbr.id = item->vector_id;
            nbr.distance = tmp_distance;
            new_vector_push_back(full_retset, &nbr);

            // 检查邻居
            for (uint32_t m = 0; m < n_count; m++)
            {
                uint32_t id = item->neighbors[m];
                if (!test_bit(is_visited, (size_t)id))
                {
                    // 如果没访问过
                    // 计算距离 应该是计算与查询向量的距离
                    float distance = get_distance_to_target_by_id(id, target, target->dim);
                    Neighbor nn;
                    nn.id = id;
                    nn.distance = distance;
                    nn.expanded = false;
                    set_bit(is_visited, (size_t)id);
                    // 将邻居加入候选集
                    priority_queue_insert(retset, nn);
                }
            }
        }
        new_vector_clear(frontier_nhoods);
    }
    elog(INFO, "total_visit_size: %ld", total_visit_size);
    // 排序full_retset
    bool use_full_sort = true;
    new_vector_sort(full_retset, compare_neighbors);
    if (use_full_sort)
    {
        // 隐式重排序
        // 将full_retset的距离替换为真实距离

        // 截断一部分节点
        if (full_retset->size > k * FULL_PRECISION_REORDER_MULTIPLIER)
        {
            new_vector_resize(full_retset, k * FULL_PRECISION_REORDER_MULTIPLIER);
        }

        // kv_table *my_kv_table = create_kv_table();
        new_vector_clear(frontier_nhoods);
        new_vector_clear(frontier_nhoods_req);

        for (size_t i = 0; i < full_retset->size; i++)
        {
            // 判断哪些在内存哪些不在
            // 先全部从磁盘中获取
            uint32_t n_id = ((Neighbor *)new_vector_get(full_retset, i))->id;
            // elog(INFO, "isert_id: %d", n_id);
            //  int ret = kv_insert(my_kv_table, n_id, i);
            //  if (ret == 0)
            //  {
            //      elog(ERROR, "kv_insert failed");
            //  }
            new_vector_push_back(frontier_nhoods_req, &n_id);
        }

        new_get_vectors(index_table_name, frontier_nhoods, frontier_nhoods_req);

        if (frontier_nhoods->size != full_retset->size)
        {
            elog(ERROR, "frontier_nhoods->size != full_retset->size");
        }

        for (size_t i = 0; i < full_retset->size; i++)
        {
            VectorCache *cur = new_vector_get(frontier_nhoods, i);
            // elog(INFO, "isert_id: %d", cur->vector_id);
            float true_dist = vector_L2_distance(target->dim, target->x, cur->vector->x);
            Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
            nbr->distance = true_dist;
        }
    }

    new_vector_sort(full_retset, compare_neighbors);

    for (uint32_t i = 0; i < k && i < full_retset->size; i++)
    {
        Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
        // elog(INFO, "id: %d, distance: %f", nbr->id, nbr->distance);
        uint32_t nbr_id = nbr->id;
        float dist = nbr->distance;
        elog(INFO, "id: %d, distance: %f", nbr_id, dist);
        new_vector_push_back(res_vector_ids, &nbr_id);
    }

    free(is_visited);
    new_vector_free(frontier);
    new_vector_free(frontier_nhoods);
    new_vector_free(frontier_nhoods_req);
    new_vector_free(full_retset);
    // new_vector_free(vector_caches);
    // pfree(vector_caches);
    free_queue(retset);
    pfree(frontier);
    pfree(frontier_nhoods);
    pfree(frontier_nhoods_req);
    pfree(retset);
    pfree(full_retset);

    return res_vector_ids;
}

NewVector *enhanced_search_k_nearest_neighbors(char *index_table_name, NewVector *init_ids,
                                               int k, Vector *target, uint32_t vector_num)
{
    // 要考虑的点，邻居肯定不是全加载
    // （可选） 预先加载三跳以内的向量和邻居

    // 加载PQ查表和压缩向量
    // 选择起点 就是建索引的点

    // 初始化数据结构
    // visited = new bool[vector_num];
    // 优先队列
    // 一个存储Neighbor的全部候选节点
    size_t total_visit_size = 0;

    uint32_t io_limit = 100000;
    uint32_t num_ios = 0;
    uint32_t beam_width = 35;

    NeighborPriorityQueue *retset;
    uint32_t *is_visited;
    NewVector *full_retset;
    NewVector *frontier;
    // 存储准备查询的节点
    NewVector *frontier_nhoods;
    NewVector *frontier_nhoods_req;
    // NewVector *vector_caches;
    //  缓存机制

    NewVector *res_vector_ids;

    uint32_t retset_size = 90;

    retset = (NeighborPriorityQueue *)palloc(sizeof(NeighborPriorityQueue));
    init_queue(retset, retset_size);
    is_visited = (uint32_t *)calloc(vector_num / 32 + 1, sizeof(uint32_t));
    full_retset = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init(full_retset, sizeof(Neighbor));
    frontier = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods_req = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods_req, sizeof(uint32_t), 2 * beam_width);

    frontier_nhoods = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(frontier_nhoods, sizeof(VectorCache), 2 * beam_width);

    res_vector_ids = (NewVector *)palloc(sizeof(NewVector));
    new_vector_init_with_capacity(res_vector_ids, sizeof(uint32_t), k + 1);
    // vector_caches = (NewVector *)palloc(sizeof(NewVector));
    // new_vector_init_with_capacity(vector_caches, sizeof(VectorCache), 3 * beam_width);

    // 获取初始节点的id和dist 加入候选集
    for (size_t i = 0; i < init_ids->size; i++)
    {
        uint32_t *init_id_pointer = new_vector_get(init_ids, i);
        uint32_t init_id = *init_id_pointer;
        Vector *init_vec = InitVector(target->dim);
        get_vector_in_database("vectors_index_table", init_id, init_vec);
        float dist = vector_L2_distance(target->dim, target->x, init_vec->x);
        Neighbor init_node;
        init_node.id = init_id;
        init_node.distance = dist;
        priority_queue_insert(retset, init_node);
        set_bit(is_visited, (size_t)init_id);
    }

    // 先不用缓存
    while (has_unexpanded_node(retset) && num_ios < io_limit)
    {
        // new_beam
        uint32_t num_seen = 0;
        new_vector_clear(frontier);
        // frontier->size = 0;
        new_vector_clear(frontier_nhoods_req);
        new_vector_clear(frontier_nhoods);
        while (has_unexpanded_node(retset) && frontier->size < beam_width && num_seen < beam_width)
        {
            Neighbor nbr = closest_unexpanded(retset);
            num_seen++;
            uint32_t temp_id = nbr.id;
            // 判断该节点的邻居是否已经被缓存
            // 是 直接使用缓存的邻居数据
            // 否 加入frontier 准备缓存
            if (false)
            {
            }
            else
            {
                // frontier.push_back(nbr.id);
                new_vector_push_back(frontier, &temp_id);
                // elog(INFO, "frontier: %u", temp_id);
            }
        }

        if (frontier->size != 0)
        {
            for (uint32_t i = 0; i < frontier->size; i++)
            {
                // 准备加载frontier中的节点的邻居
                uint32_t *current_id_pointer = new_vector_get(frontier, i);
                // DEBUG 这里会不会有内存分配的问题
                new_vector_push_back(frontier_nhoods_req, current_id_pointer);
                num_ios++;
            }
        }

        // 加载frontier中的节点的邻居
        // select vector and neighbors from disk
        new_get_vectors_and_neighbors(index_table_name, frontier_nhoods, frontier_nhoods_req);
        total_visit_size += frontier_nhoods_req->size;
        new_vector_clear(frontier_nhoods_req);
        // new_vector_append(vector_caches, frontier_nhoods);

        // 处理缓存中的邻居
        // 拿出一个节点
        // 计算与查询向量的PQ距离
        // 将当前节点加入full_retset
        // 拿出这个节点的邻居
        // 计算与查询向量的PQ距离
        // 加入best_l_node

        // 循环frontier_nhoods
        // 获取邻居数量和坐标
        // 计算当前节点与查询向量的距离
        // 处理邻居节点
        for (uint32_t i = 0; i < frontier_nhoods->size; i++)
        {
            VectorCache *item = (VectorCache *)new_vector_get(frontier_nhoods, i);
            // uint32_t id = item->vector_id;
            uint32_t n_count = item->neighbor_count;
            Vector *item_vec = item->vector;
            float tmp_distance = vector_L2_distance(target->dim, target->x, item_vec->x);
            Neighbor nbr;
            nbr.id = item->vector_id;
            nbr.distance = tmp_distance;
            new_vector_push_back(full_retset, &nbr);

            // 检查邻居
            for (uint32_t m = 0; m < n_count; m++)
            {
                uint32_t id = item->neighbors[m];
                if (!test_bit(is_visited, (size_t)id))
                {
                    // 如果没访问过
                    // 计算距离 应该是计算与查询向量的距离
                    float distance = get_distance_to_target_by_id(id, target, target->dim);
                    Neighbor nn;
                    nn.id = id;
                    nn.distance = distance;
                    nn.expanded = false;
                    set_bit(is_visited, (size_t)id);
                    // 将邻居加入候选集
                    priority_queue_insert(retset, nn);
                }
            }
        }
        new_vector_clear(frontier_nhoods);
    }
    elog(INFO, "total_visit_size: %ld", total_visit_size);
    // 排序full_retset
    bool use_full_sort = true;
    if (use_full_sort)
    {
        // 隐式重排序
        // 将full_retset的距离替换为真实距离

        // 截断一部分节点
        if (full_retset->size > k * FULL_PRECISION_REORDER_MULTIPLIER)
        {
            new_vector_resize(full_retset, k * FULL_PRECISION_REORDER_MULTIPLIER);
        }

        // kv_table *my_kv_table = create_kv_table();
        new_vector_clear(frontier_nhoods);
        new_vector_clear(frontier_nhoods_req);

        for (size_t i = 0; i < full_retset->size; i++)
        {
            // 判断哪些在内存哪些不在
            // 先全部从磁盘中获取
            uint32_t n_id = ((Neighbor *)new_vector_get(full_retset, i))->id;
            // elog(INFO, "isert_id: %d", n_id);
            //  int ret = kv_insert(my_kv_table, n_id, i);
            //  if (ret == 0)
            //  {
            //      elog(ERROR, "kv_insert failed");
            //  }
            new_vector_push_back(frontier_nhoods_req, &n_id);
        }

        new_get_vectors(index_table_name, frontier_nhoods, frontier_nhoods_req);

        if (frontier_nhoods->size != full_retset->size)
        {
            elog(ERROR, "frontier_nhoods->size != full_retset->size");
        }

        for (size_t i = 0; i < full_retset->size; i++)
        {
            VectorCache *cur = new_vector_get(frontier_nhoods, i);
            // elog(INFO, "isert_id: %d", cur->vector_id);
            float true_dist = vector_L2_distance(target->dim, target->x, cur->vector->x);
            Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
            nbr->distance = true_dist;
        }
    }

    new_vector_sort(full_retset, compare_neighbors);

    for (uint32_t i = 0; i < k && i < full_retset->size; i++)
    {
        Neighbor *nbr = (Neighbor *)new_vector_get(full_retset, i);
        // elog(INFO, "id: %d, distance: %f", nbr->id, nbr->distance);
        uint32_t nbr_id = nbr->id;
        float dist = nbr->distance;
        elog(INFO, "id: %d, distance: %f", nbr_id, dist);
        new_vector_push_back(res_vector_ids, &nbr_id);
    }

    free(is_visited);
    new_vector_free(frontier);
    new_vector_free(frontier_nhoods);
    new_vector_free(frontier_nhoods_req);
    new_vector_free(full_retset);
    // new_vector_free(vector_caches);
    // pfree(vector_caches);
    free_queue(retset);
    pfree(frontier);
    pfree(frontier_nhoods);
    pfree(frontier_nhoods_req);
    pfree(retset);
    pfree(full_retset);

    return res_vector_ids;
}

/*
 * Get proc
 */
FmgrInfo *
VamanaOptionalProcInfo(Relation index, uint16 procnum)
{
    if (!OidIsValid(index_getprocid(index, 1, procnum)))
        return NULL;

    return index_getprocinfo(index, 1, procnum);
}

PGDLLEXPORT Datum l2_normalize(PG_FUNCTION_ARGS);

/*
 * Get type info
 */
const VamanaTypeInfo *
VamanaGetTypeInfo(Relation index)
{
    // 检查是否支持该函数
    // FmgrInfo *procinfo = VamanaOptionalProcInfo(index, VAMANA_TYPE_INFO_PROC);
    FmgrInfo *procinfo = NULL;

    if (procinfo == NULL)
    {
        static const VamanaTypeInfo typeInfo = {
            .maxDimensions = VAMANA_MAX_DIM,
            .normalize = l2_normalize,
            .checkValue = NULL};

        return (&typeInfo);
    }
    else
        return (const VamanaTypeInfo *)DatumGetPointer(FunctionCall0Coll(procinfo, InvalidOid));
}

void MyPrintVector(Vector *vector)
{
    float *arr = vector->x;
    char temp[64];
    size_t out_size = 2000;
    char out[2000];
    size_t size = vector->dim;

    for (size_t i = 0; i < size; i++)
    {
        snprintf(temp, sizeof(temp), "%.4f", arr[i]);
        strncat(out, temp, out_size - strlen(out) - 1);

        if (i < out_size - 1)
        {
            strncat(out, ", ", out_size - strlen(out) - 1);
        }
    }
    elog(LOG, "vector: %s", out);
}

void new_vector_to_string(NewVector *vec, char *out, size_t out_size)
{
    char temp[32]; // 用于存储单个元素转化为字符串的临时缓冲区
    out[0] = '\0'; // 初始化为空字符串

    for (size_t i = 0; i < vec->size; i++)
    {
        uint32_t *val = (uint32_t *)new_vector_get(vec, i);
        snprintf(temp, sizeof(temp), "%u", *val); // 将 uint32_t 转为字符串

        // 拼接到最终字符串
        strncat(out, temp, out_size - strlen(out) - 1);

        // 添加逗号和空格分隔符（如果不是最后一个元素）
        if (i < vec->size - 1)
        {
            strncat(out, ", ", out_size - strlen(out) - 1);
        }
    }
}