#ifndef VAMANA_H
#define VAMANA_H

#include "postgres.h"
#include "access/generic_xlog.h"
#include "storage/bufmgr.h"
#include "utils/relptr.h"

/* Vamana 特定的参数 */
#define VAMANA_MAX_LEVEL 64      // 最大层数
#define VAMANA_DEFAULT_R 64      // 默认候选集大小
#define VAMANA_DEFAULT_L 100     // 默认搜索列表大小
#define VAMANA_DEFAULT_ALPHA 1.2 // 默认alpha参数

/* Vamana元素结构 */
typedef struct VamanaElementData
{
    ItemPointerData heaptid; // 堆表指针
    BlockNumber blkno;       // 块号
    OffsetNumber offno;      // 偏移量
    int16 level;             // 层级
    RelativePtr value;       // 向量值
    RelativePtr neighbors;   // 邻居列表
    uint32 hash;             // 哈希值
} VamanaElementData;

typedef VamanaElementData *VamanaElement;

/* Vamana 构建状态 */
typedef struct VamanaBuildState
{
    Relation heap;
    Relation index;
    IndexInfo *indexInfo;

    // 构建参数
    int r;        // 候选集大小
    int l;        // 搜索列表大小
    double alpha; // alpha参数

    // 内存管理
    MemoryContext tmpCtx;
    MemoryContext graphCtx;

    // 并行构建支持
    bool isParallel;
    int workers;

    // 统计信息
    double reltuples;
    double indtuples;
} VamanaBuildState;

/* 函数声明 */
extern void VamanaInit(void);
extern IndexBuildResult *VamanaBuild(Relation heap, Relation index,
                                     IndexInfo *indexInfo);
extern bool VamanaInsert(Relation index, Datum *values, bool *isnull,
                         ItemPointer heap_tid, Relation heap,
                         IndexUniqueCheck checkUnique);
extern IndexScanDesc VamanaBeginscan(Relation index, int nkeys, int norderbys);
extern void VamanaRescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                         ScanKey orderbys, int norderbys);
extern bool VamanaGettuple(IndexScanDesc scan, ScanDirection dir);
extern void VamanaEndscan(IndexScanDesc scan);

#endif /* VAMANA_H */