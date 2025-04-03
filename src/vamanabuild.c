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
#include "vamana.h"

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
 * 在内存中插入元素
 */
static void
InsertElementInMemory(VamanaBuildState *buildstate, VamanaElement element)
{
}

/*
 * 插入元组
 */
static bool
InsertTuple(Relation index, Datum *values, bool *isnull,
            ItemPointer heaptid, VamanaBuildState *buildstate)
{
    // VamanaGraph *graph = buildstate->graph;
    // VamanaElement element;
    // Size valueSize;
    // Datum value;
    // char *base = buildstate->vamanaarea;

    // /* 跳过空值 */
    // if (isnull[0])
    //     return false;

    // /* 获取索引值 */
    // if (!VamanaFormIndexValue(&value, values, isnull, buildstate))
    //     return false;

    // valueSize = VARSIZE_ANY(DatumGetPointer(value));

    // /* 检查内存使用情况 */
    // LWLockAcquire(&graph->flushLock, LW_SHARED);
    // if (graph->memoryUsed >= graph->memoryTotal)
    // {
    //     LWLockRelease(&graph->flushLock);
    //     LWLockAcquire(&graph->flushLock, LW_EXCLUSIVE);

    //     if (!graph->flushed)
    //     {
    //         ereport(NOTICE,
    //                 (errmsg("vamana graph no longer fits into maintenance_work_mem"),
    //                  errhint("Increase maintenance_work_mem to speed up builds.")));

    //         FlushPages(buildstate);
    //     }

    //     LWLockRelease(&graph->flushLock);
    //     return VamanaInsertTupleOnDisk(index, &buildstate->support,
    //                                    value, heaptid);
    // }

    // /* 分配新元素 */
    // element = VamanaInitElement(base, heaptid, valueSize,
    //                             &buildstate->allocator);

    // /* 复制数据 */
    // memcpy(VamanaPtrAccess(base, element->value),
    //        DatumGetPointer(value), valueSize);

    // /* 插入元素 */
    // InsertElementInMemory(buildstate, element);

    // LWLockRelease(&graph->flushLock);
    return true;
}

/*
 * 构建回调函数
 */
static void
BuildCallback(Relation index, ItemPointer tid, Datum *values,
              bool *isnull, bool tupleIsAlive, void *state)
{
    // VamanaBuildState *buildstate = (VamanaBuildState *)state;
    // VamanaGraph *graph = buildstate->graph;
    // MemoryContext oldCtx;

    // oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

    // if (InsertTuple(index, values, isnull, tid, buildstate))
    // {
    //     SpinLockAcquire(&graph->lock);
    //     graph->indtuples++;
    //     SpinLockRelease(&graph->lock);
    // }

    // MemoryContextSwitchTo(oldCtx);
    // MemoryContextReset(buildstate->tmpCtx);
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

    buildstate->dimensions = 128;
    buildstate->R = 32;
    buildstate->L = 64;
    buildstate->B = 1024;
    buildstate->M = 1024;
    buildstate->T = 1;

    buildstate->reltuples = 0;
    buildstate->indtuples = 0;

    // TODO 检查参数
}
/*
 * Build graph
 */
static void BuildGraph(VamanaBuildState *buildstate, ForkNumber forkNum)
{
    // 解析参数
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

    BuildGraph(buildstate, forkNum);

    if (RelationNeedsWAL(index) || forkNum == INIT_FORKNUM)
        log_newpage_range(index, forkNum, 0, RelationGetNumberOfBlocksInFork(index, forkNum), true);

    FreeBuildState(buildstate);
}

IndexBuildResult *
vamanabuild(Relation heap, Relation index, IndexInfo *indexInfo)
{

    IndexBuildResult *result;
    VamanaBuildState buildstate;

    BuildIndex(heap, index, indexInfo, &buildstate, MAIN_FORKNUM);

    result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    result->heap_tuples = buildstate.reltuples;
    result->index_tuples = buildstate.indtuples;

    return result;
}

/* 空索引构建函数 */
void vamanabuildempty(Relation index)
{
    IndexInfo *indexInfo = BuildIndexInfo(index);
    VamanaBuildState buildstate;
    BuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}