#include "postgres.h"
#include "vamana.h"
#include "fmgr.h"
#include "utils/geo_decls.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h" // 包含操作符的定义
#include "access/htup_details.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "catalog/pg_class.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "executor/spi.h"
#include "pgstat.h"
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
#include <time.h>
#include <stdlib.h>
/* Vamana图搜索 */
static List *
SearchVamanaGraph(Relation index, Datum query, int k)
{
    // // 获取入口点
    // VamanaElement entryPoint = GetEntryPoint(index);

    // 初始化结果集
    List *results = NIL;

    // // 初始化访问标记
    // visited_hash *visited = InitVisitedHash();

    // // 从最高层开始搜索
    // for (int level = entryPoint->level; level >= 0; level--)
    // {
    //     // 在当前层搜索k近邻
    //     results = SearchLayer(query, entryPoint, k, level, visited);

    //     // 更新入口点为当前层最近邻
    //     entryPoint = GetNearestNeighbor(results);
    // }

    // // 清理
    // FreeVisitedHash(visited);

    return results;
}

// 获取表的 Relation 结构
Relation get_relation_by_name(const char *table_name)
{
    // 通过表名获取 Relation
    Oid relid = get_relname_relid(table_name, InvalidOid);
    if (!OidIsValid(relid))
    {
        elog(ERROR, "Relation %s does not exist", table_name);
        return NULL;
    }

    // 打开 Relation
    Relation rel = relation_open(relid, AccessShareLock);
    return rel;
}

void get_heaptids_from_table(const char *table_name, NewVector *vec_ids, NewVector *heaptids)
{

    // 使用 IN 子句构建查询条件
    StringInfoData query;
    initStringInfo(&query);
    appendStringInfo(&query, "SELECT ctid, (id-1) AS vector_id FROM %s WHERE id IN (", table_name);

    // 构建 IN 子句，加入所有 vector_id
    for (int i = 0; i < vec_ids->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(vec_ids, i);
        uint32_t vec_id = *vector_id_pointer + 1;
        if (i > 0)
            appendStringInfo(&query, ",");
        appendStringInfo(&query, "%d", vec_id);
    }
    appendStringInfo(&query, ") ORDER BY ARRAY_POSITION(ARRAY[");

    // 添加 vector_ids 顺序
    for (int i = 0; i < vec_ids->size; i++)
    {
        uint32_t *vector_id_pointer = new_vector_get(vec_ids, i);
        uint32_t vec_id = *vector_id_pointer + 1;
        if (i > 0)
            appendStringInfo(&query, ",");
        appendStringInfo(&query, "%d", vec_id);
    }
    appendStringInfo(&query, "], id);");

    // 执行查询
    SPI_connect();
    SPI_exec(query.data, 0); // 执行查询
    elog(INFO, "SPI_exec query: %s", query.data);

    // 获取查询结果
    if (SPI_processed > 0)
    {
        for (int i = 0; i < SPI_processed; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            ItemPointer tempheaptid = &tuple->t_self; // 获取 ctid (heaptid)

            bool isNull = false;
            // 获取 heaptid
            Datum ctidDatum = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isNull); // 获取 ctid
            if (isNull)
            {
                elog(ERROR, "vector_id is null");
            }
            ItemPointer heaptid = DatumGetItemPointer(ctidDatum);
            int vector_id = DatumGetInt32(SPI_getbinval(tuple, SPI_tuptable->tupdesc, 2, &isNull));
            if (isNull)
            {
                elog(ERROR, "vector_id is null");
            }
            elog(INFO, "For vector_id %d, ctid (heaptid): (%u, %u)",
                 vector_id, ItemPointerGetBlockNumber(heaptid), ItemPointerGetOffsetNumber(heaptid));
            new_vector_push_back(heaptids, &heaptid);
        }
    }

    // 结束 SPI 会话
    SPI_finish();
}

// // 获取表中的 ctid（heaptid）
// void get_heaptid_from_table(const char *table_name, int vector_id)
// {
//     // 获取 Relation
//     Relation rel = get_relation_by_name(table_name);

//     // 创建 HeapScan 描述符
//     HeapScanDesc scan = heap_beginscan(rel, SnapshotSelf, 0, NULL);

//     // 构建扫描条件：根据 vector_id 查找
//     ScanKeyData scanKey;
//     ScanKeyInit(&scanKey, 1, BTEqualStrategyNumber, F_OIDEQ, Int32GetDatum(vector_id));

//     // 执行扫描
//     heap_rescan(scan, &scanKey, 1, NULL, 0);

//     // 获取扫描结果
//     HeapTuple tuple;
//     while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
//     {
//         // 获取 ctid（heaptid）
//         ItemPointer heaptid = &tuple->t_self;
//         elog(INFO, "Found heaptid (ctid) for vector_id %d: (%u, %u)",
//              vector_id, ItemPointerGetBlockNumber(heaptid), ItemPointerGetOffsetNumber(heaptid));
//     }

//     // 结束扫描
//     heap_endscan(scan);

//     // 关闭 Relation
//     relation_close(rel, AccessShareLock);
// }

/*
 * Get scan value
 */
static Datum
GetScanValue(IndexScanDesc scan)
{
    VamanaScanOpaque so = (VamanaScanOpaque)scan->opaque;
    Datum value;

    if (scan->orderByData->sk_flags & SK_ISNULL)
        value = PointerGetDatum(NULL);
    else
    {
        elog(INFO, "GetScanValue");
        value = scan->orderByData->sk_argument;
        Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
        Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));
    }

    return value;
}

size_t calculate_search_entry(size_t num_points)
{
    // 生成一个大范围的随机数 r
    size_t r = (size_t)rand() * (size_t)RAND_MAX + (size_t)rand();

    // 返回随机索引，确保它在容量范围内
    return (uint32_t)(r % num_points);
}

/* 扫描函数 */
bool vamanagettuple(IndexScanDesc scan, ScanDirection dir)
{
    VamanaScanOpaque so = (VamanaScanOpaque)scan->opaque;
    // MemoryContext oldCtx = MemoryContextSwitchTo(so->tmpCtx);

    NewVector *heaptids = NULL;
    // Datum value;
    //  初始化返回结果
    if (so->first)
    {
        // 执行搜索函数

        // value = GetScanValue(scan);
        // if (value == PointerGetDatum(NULL))
        // {
        //     elog(INFO, "GetScanValue is null");
        // }
        // else
        // {
        //     elog(INFO, "GetScanValue is not null");
        //     Vector *query_vec = (Vector *)DatumGetVector(value);
        //     elog(INFO, "target vector:");
        //     char *msg;
        //     PrintVector(msg, query_vec);
        //     elog(INFO, "query vector: %s", msg);
        // }

        Vector *target = InitVector(128);
        heaptids = (NewVector *)palloc(sizeof(NewVector));
        const char *table_name = "vectors_index_table";
        srand(time(NULL)); // 用当前时间作为随机种子
        for (uint32_t i = 0; i < 128; i++)
        {
            target->x[i] = ((float)rand() / RAND_MAX) * 50.0f; // 生成 [0.0, 100.0)
        }
        uint32_t init_id = calculate_search_entry(10000);
        uint32_t init_id2 = calculate_search_entry(10000);
        NewVector *init_ids = (NewVector *)palloc(sizeof(NewVector));
        new_vector_init_with_capacity(init_ids, sizeof(uint32_t), 2);
        new_vector_push_back(init_ids, &init_id);
        new_vector_push_back(init_ids, &init_id2);
        uint32_t k = 10;
        NewVector *target_nbrs = new_search_k_nearest_neighbors(table_name, init_ids, k, target, 10000);
        const char *origin_table_name = "vectors";
        new_vector_init_with_capacity(heaptids, sizeof(ItemPointer), k + 1);
        get_heaptids_from_table(origin_table_name, target_nbrs, heaptids);
        so->heaptids_vectors = heaptids;
        // 设置扫描状态
        so->first = false;
        so->current_idx = 0;
    }

    // 开始返回
    if (so->current_idx < so->heaptids_vectors->size)
    {
        // 根据扫描方向，返回正确的元组
        ItemPointer *current_heaptid = new_vector_get(so->heaptids_vectors, so->current_idx);

        // 增加索引
        so->current_idx++;

        // 返回结果
        scan->xs_heaptid = **current_heaptid;
        scan->xs_recheck = false;
        scan->xs_recheckorderby = false;
        elog(INFO, "Returning heaptid: %p", *current_heaptid);
        return true;
    }

    elog(INFO, "vamana gettuple finished");
    return false;
}

/* 开始扫描 */
IndexScanDesc
vamanabeginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    VamanaScanOpaque so;
    double maxMemory;

    // 创建扫描描述符
    scan = RelationGetIndexScan(index, nkeys, norderbys);

    // 分配扫描私有数据
    so = (VamanaScanOpaque)palloc(sizeof(VamanaScanOpaqueData));
    so->first = true;
    so->typeInfo = VamanaGetTypeInfo(index);

    /*
     * Use a lower max allocation size than default to allow scanning more
     * tuples for iterative search before exceeding work_mem
     */
    so->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
                                       "vamana scan temporary context",
                                       0, 8 * 1024, 256 * 1024 * 1024);

    /* Calculate max memory */
    /* Add 256 extra bytes to fill last block when close */
    maxMemory = (double)work_mem * 1024.0 * 1024.0 + 256;
    so->maxMemory = Min(maxMemory, (double)SIZE_MAX);

    scan->opaque = so;
    elog(INFO, "vamana beginscan");
    return scan;
}

/* 重新扫描 */
void vamanarescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                  ScanKey orderbys, int norderbys)
{
    // VamanaScanOpaque so = (VamanaScanOpaque)scan->opaque;

    // // 重置扫描状态
    // so->first = true;

    // if (keys && scan->numberOfKeys > 0)
    //     memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));

    // if (orderbys && scan->numberOfOrderBys > 0)
    //     memmove(scan->orderByData, orderbys,
    //             scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/* 结束扫描 */
void vamanaendscan(IndexScanDesc scan)
{
    VamanaScanOpaque so = (VamanaScanOpaque)scan->opaque;

    // 释放资源
    if (so->tmpCtx)
        MemoryContextDelete(so->tmpCtx);

    pfree(so);
    scan->opaque = NULL;
}
