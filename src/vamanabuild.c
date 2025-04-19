#include "postgres.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include "access/parallel.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "commands/progress.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "catalog/namespace.h"
#include "utils/rel.h"
#include "vamana.h"
#include "diskann.h"

/* 共享内存键值 */
#define PARALLEL_KEY_VAMANA_SHARED UINT64CONST(0xB000000000000001)
#define PARALLEL_KEY_VAMANA_AREA UINT64CONST(0xB000000000000002)
#define PARALLEL_KEY_QUERY_TEXT UINT64CONST(0xB000000000000003)

#define TYPE float // 这里可以根据需要定义类型

// 互斥锁数组
pthread_mutex_t *node_locks;

// 错误码定义
#define ANN_SUCCESS 0
#define ANN_ERROR -1

static void
CreateMetaPage(VamanaBuildState *buildstate)
{
    Relation index = buildstate->index;
    ForkNumber forkNum = buildstate->forkNum;
    Buffer buf;
    Page page;
    VamanaMetaPage metap;

    buf = VamanaNewBuffer(index, forkNum);
    page = BufferGetPage(buf);
    VamanaInitPage(buf, page);

    /* Set metapage data */
    metap = VamanaPageGetMeta(page);
    metap->magicNumber = VAMANA_MAGIC_NUMBER;
    metap->version = VAMANA_VERSION;
    metap->dimensions = buildstate->dimensions;
    metap->entryBlkno = InvalidBlockNumber;
    metap->entryOffno = InvalidOffsetNumber;
    metap->entryLevel = -1;
    metap->insertPage = InvalidBlockNumber;
    ((PageHeader)page)->pd_lower =
        ((char *)metap + sizeof(VamanaMetaPageData)) - (char *)page;

    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

/*
 * Flush pages
 */
static void
FlushPages(VamanaBuildState *buildstate)
{

    CreateMetaPage(buildstate);

    buildstate->graph->flushed = true;
    MemoryContextReset(buildstate->graphCtx);
}

/*
 * Free resources
 */
static void
FreeBuildState(VamanaBuildState *buildstate)
{
    MemoryContextDelete(buildstate->graphCtx);
    MemoryContextDelete(buildstate->tmpCtx);
}

/*
 * Memory context allocator
 */
static void *
VamanaMemoryContextAlloc(Size size, void *state)
{
    VamanaBuildState *buildstate = (VamanaBuildState *)state;
    void *chunk = MemoryContextAlloc(buildstate->graphCtx, size);

    buildstate->graphData.memoryUsed = MemoryContextMemAllocated(buildstate->graphCtx, false);

    return chunk;
}

/*
 * Initialize the graph
 */
static void
InitGraph(VamanaGraph *graph, char *base, Size memoryTotal)
{
    elog(INFO, "InitGraph");
    graph->memoryUsed = 0;
    graph->memoryTotal = memoryTotal;
    graph->flushed = false;
    graph->indtuples = 0;
    SpinLockInit(&graph->lock);
    elog(INFO, "InitGraph end");
}

/*
 * 插入元组
 */
static bool
InsertTuple(Relation index, Datum *values, bool *isnull,
            ItemPointer heaptid, VamanaBuildState *buildstate)
{
    HeapTuple tuple;
    TupleDesc tupdesc;
    Oid table_oid;
    MemoryContext oldContext;

    // 获取表的描述信息
    tupdesc = RelationGetDescr(index);

    // 根据表的描述和输入的 Datum 构建一个 HeapTuple
    tuple = heap_form_tuple(tupdesc, values, isnull);

    // 获取表的 OID
    table_oid = RelationGetRelid(index);

    // 锁定表，准备插入
    // LWLockAcquire(RelationGetRelationLock(index), LW_EXCLUSIVE);

    // 执行插入操作
    heap_insert(index, tuple, GetCurrentCommandId(true), 0, NULL);

    // 更新传入的 ItemPointer，指向插入的元组位置
    ItemPointerSet(heaptid,
                   BlockIdGetBlockNumber(&(tuple->t_self.ip_blkid)),
                   tuple->t_self.ip_posid);

    // 释放锁
    // LWLockRelease(RelationGetRelationLock(index));

    // 释放内存
    heap_freetuple(tuple);

    return true;
}

/*
 * 构建回调函数
 */
static void
BuildCallback(Relation index, ItemPointer tid, Datum *values,
              bool *isnull, bool tupleIsAlive, void *state)
{
    VamanaBuildState *buildstate = (VamanaBuildState *)state;
    VamanaGraph *graph = buildstate->graph;
    MemoryContext oldCtx;

    oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

    // if (InsertTuple(index, values, isnull, tid, buildstate))
    // {
    //     SpinLockAcquire(&graph->lock);
    //     graph->indtuples++;
    //     SpinLockRelease(&graph->lock);
    // }

    MemoryContextSwitchTo(oldCtx);
    MemoryContextReset(buildstate->tmpCtx);
}

/*
 * 做构建前的准备
 */
static void
InitBuildState(VamanaBuildState *buildstate, Relation heap, Relation index, IndexInfo *indexInfo, ForkNumber forkNum)
{
    buildstate->heap = heap;
    buildstate->index = index;
    buildstate->indexInfo = indexInfo;
    buildstate->forkNum = forkNum;
    // vectors_index_table
    buildstate->index_table_name = psprintf("%s_index_table", RelationGetRelationName(heap));
    buildstate->data_table_name = pstrdup(RelationGetRelationName(heap));

    buildstate->dimensions = 128;
    buildstate->R = 32;
    buildstate->L = 64;
    buildstate->B = 1024;
    buildstate->M = 1024;
    buildstate->T = 1;

    buildstate->reltuples = 0;
    buildstate->indtuples = 0;

    InitGraph(&buildstate->graphData, NULL, (Size)maintenance_work_mem * 1024L);
    buildstate->graph = &buildstate->graphData;
    buildstate->graphCtx = GenerationContextCreate(CurrentMemoryContext,
                                                   "Vamana build graph context",
#if PG_VERSION_NUM >= 150000
                                                   1024 * 1024, 1024 * 1024,
#endif
                                                   1024 * 1024);
    buildstate->tmpCtx = AllocSetContextCreate(CurrentMemoryContext,
                                               "Vamana build temporary context",
                                               ALLOCSET_DEFAULT_SIZES);
    // TODO 检查参数
}
/*
 * Build graph
 */
static void BuildGraph(VamanaBuildState *buildstate, ForkNumber forkNum)
{
    int parallel_workers = 0;
    //pgstat_progress_update_param(PROGRESS_CREATEIDX_SUBPHASE, PROGRESS_VAMANA_PHASE_LOAD);

    // /* Add tuples to graph */
    // if (buildstate->heap != NULL)
    // {

    //     buildstate->reltuples = table_index_build_scan(buildstate->heap, buildstate->index, buildstate->indexInfo,
    //                                                    true, true, BuildCallback, (void *)buildstate, NULL);

    //     buildstate->indtuples = buildstate->graph->indtuples;
    // }
    // 开始转存
    bool ret = create_index_table(buildstate->index_table_name, buildstate->data_table_name, buildstate->dimensions);
    if (!ret)
    {
        elog(ERROR, "create index table failed");
    }
    //elog(INFO, "Hello, Vamana!");
    size_t slice_size = 0;

    // 加载向量数据
    float *storage = NULL;
    // load_vector_data("vectors", "embedding", &npt_val, &dim_val);
    // elog(INFO,"npt_val = %d, dim_val = %d", npt_val, dim_val);
    //  gen_random_slice(aa, npt_val, dim_val, 0.01, &storage, &slice_size);
    const char *dataFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_base.fbin";
    const char *indexFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/test";
    // R 32 邻居数 L 64 最大候选集大小
    const char *buildParams = "32 50 200 1 1";
    enum diskann_metric_t metric = DISKANN_L2; // 假设使用 L2 作为度量方式
    int use_opq = false;                       // 启用 OPQ
    const char *codebookPrefix = "/path/to/codebook";
    int status = build_disk_index(dataFile, indexFile, buildParams, metric, use_opq, codebookPrefix, buildstate->index_table_name, "embedding");
    // if (!buildstate->graph->flushed)
    //     FlushPages(buildstate);
}

/*
 * Build the index
 */
/*
 * Build a Vamana graph index for the given heap relation.
 *
 * This function initializes the build state, constructs the Vamana graph,
 * and handles WAL logging of the new index pages. The build process uses
 * a random seed of 42 when VAMANA_MEMORY is defined.
 *
 * Parameters:
 *      heap - heap relation to build index for
 *      index - index relation to build
 *      indexInfo - information about the index
 *      buildstate - state for the index build process
 *      forkNum - fork number to build index on
 */
static void
BuildIndex(Relation heap, Relation index, IndexInfo *indexInfo,
           VamanaBuildState *buildstate, ForkNumber forkNum)
{
#ifdef VAMANA_MEMORY
    SeedRandom(42);
#endif

    InitBuildState(buildstate, heap, index, indexInfo, forkNum);
    // const char *heap_name = pstrdup(RelationGetRelationName(heap));
    // elog(INFO, "Build graph for %s", heap_name);
    BuildGraph(buildstate, forkNum);

    if (RelationNeedsWAL(index) || forkNum == INIT_FORKNUM)
        log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum), true);

    FreeBuildState(buildstate);
}

IndexBuildResult *
vamanabuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    elog(INFO, "vamanabuild()");
    IndexBuildResult *result;
    VamanaBuildState buildstate;

    BuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

    result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    result->heap_tuples = buildstate.reltuples;
    result->index_tuples = buildstate.indtuples;
    elog(INFO, "vamanabuild() end");
    return result;
}

/* 空索引构建函数 */
void vamanabuildempty(Relation index)
{
    elog(INFO, "vamanabuildempty()");
    IndexInfo *indexInfo = BuildIndexInfo(index);
    VamanaBuildState buildstate;
    BuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}