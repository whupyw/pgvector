#include "postgres.h"

#include <math.h>

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

/*
 * 创建元页面
 */
static void
CreateMetaPage(VamanaBuildState *buildstate)
{
    // Relation index = buildstate->index;
    // Buffer buf;
    // Page page;
    // VamanaMetaPage metap;

    // buf = VamanaNewBuffer(index, MAIN_FORKNUM);
    // page = BufferGetPage(buf);
    // VamanaInitPage(buf, page);

    // /* 设置元页面数据 */
    // metap = VamanaPageGetMeta(page);
    // metap->magicNumber = VAMANA_MAGIC_NUMBER;
    // metap->version = VAMANA_VERSION;
    // metap->dimensions = buildstate->dimensions;
    // metap->r = buildstate->r;
    // metap->l = buildstate->l;
    // metap->alpha = buildstate->alpha;
    // metap->entryPoint = InvalidBlockNumber;

    // MarkBufferDirty(buf);
    // UnlockReleaseBuffer(buf);
}

/*
 * 在内存中插入元素
 */
static void
InsertElementInMemory(VamanaBuildState *buildstate, VamanaElement element)
{
    // VamanaGraph *graph = buildstate->graph;
    // VamanaSupport *support = &buildstate->support;
    // VamanaElement entryPoint;
    // char *base = buildstate->vamanaarea;

    // /* 获取入口点 */
    // LWLockAcquire(&graph->entryLock, LW_SHARED);
    // entryPoint = VamanaPtrAccess(base, graph->entryPoint);

    // /* 查找候选邻居 */
    // List *candidates = VamanaFindCandidates(base, element, entryPoint,
    //                                         support, buildstate->r);

    // /* 根据alpha参数过滤候选集 */
    // List *neighbors = VamanaFilterCandidates(candidates, element,
    //                                          buildstate->alpha, support);

    // /* 更新图结构 */
    // UpdateGraphInMemory(support, element, neighbors, buildstate);

    // /* 如果需要,更新入口点 */
    // if (entryPoint == NULL ||
    //     VamanaGetDistance(support, element, entryPoint) <
    //         VamanaGetDistance(support, entryPoint, graph->entryPoint))
    // {
    //     LWLockRelease(&graph->entryLock);
    //     LWLockAcquire(&graph->entryLock, LW_EXCLUSIVE);
    //     VamanaPtrStore(base, graph->entryPoint, element);
    // }

    // LWLockRelease(&graph->entryLock);
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
    // return true;
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
 * 构建索引
 */
IndexBuildResult *
vamanabuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    // IndexBuildResult *result;
    // VamanaBuildState buildstate;

    // /* 初始化构建状态 */
    // InitBuildState(&buildstate, heap, index, indexInfo);

    // /* 创建元页面 */
    // CreateMetaPage(&buildstate);

    // /* 构建图结构 */

    // BuildGraph(&buildstate);

    // /* 返回结果 */
    // result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    // result->heap_tuples = buildstate.reltuples;
    // result->index_tuples = buildstate.indtuples;

    // return result;
}

/* 空索引构建函数 */
void vamanabuildempty(Relation index)
{
    // IndexInfo *indexInfo = BuildIndexInfo(index);
    // HnswBuildState buildstate;
    // BuildIndex(NULL, index, indexInfo, &buildstate, INIT_FORKNUM);
}