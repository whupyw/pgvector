#ifndef VAMANA_H
#define VAMANA_H

#include "postgres.h"

#include "access/genam.h"
#include "access/parallel.h"
#include "lib/pairingheap.h"
#include "nodes/execnodes.h"
#include "port.h" /* for random() */
#include "utils/relptr.h"
#include "utils/sampling.h"
#include "vector.h"

/* Vamana 特定的参数 */
#define VAMANA_MAX_LEVEL 64      // 最大层数
#define VAMANA_DEFAULT_R 64      // 默认候选集大小
#define VAMANA_DEFAULT_L 100     // 默认搜索列表大小
#define VAMANA_DEFAULT_ALPHA 1.2 // 默认alpha参数

#define PROGRESS_VAMANA_PHASE_LOAD 2

typedef struct VamanaElementData VamanaElementData;
typedef struct VamanaNeighborArray VamanaNeighborArray;

#define VamanaPtrDeclare(type, relptrtype, ptrtype) \
    relptr_declare(type, relptrtype);               \
    typedef union                                   \
    {                                               \
        type *ptr;                                  \
        relptrtype relptr;                          \
    } ptrtype

/* Pointers that can be absolute or relative */
/* Use char for DatumPtr so works with Pointer */
VamanaPtrDeclare(VamanaElementData, VamanaElementRelptr, VamanaElementPtr);
VamanaPtrDeclare(VamanaNeighborArray, VamanaNeighborArrayRelptr, VamanaNeighborArrayPtr);
VamanaPtrDeclare(VamanaNeighborArrayPtr, VamanaNeighborsRelptr, VamanaNeighborsPtr);
VamanaPtrDeclare(char, DatumRelptr, DatumPtr);

// 简化后的数据结构
typedef struct
{
    size_t capacity;
    size_t size;
    uint32_t *data;
} UIntArray;

// 构建索引所需要的数据
typedef struct
{
    // 数据集路径
    char * data_path;

    // 数据存储相关
    size_t nd;             // 数据点数量
    size_t num_frozen_pts; // 冻结点数量
    size_t data_dim;       // 数据维度

    

    // 图结构存储
    struct
    {
        UIntArray *neighbors; // 邻接列表数组
    } graph_store;

    // 标签管理
    int enable_tags;

    // 构建参数
    uint32_t indexingRange;
    uint32_t indexingQueueSize;
    uint32_t indexingMaxC;

    // 状态标志
    int has_built;
} VAMANAIndex;

/* Vamana元素结构 */
typedef struct VamanaElementData
{
    ItemPointerData heaptid;      // 堆表指针
    BlockNumber blkno;            // 块号
    OffsetNumber offno;           // 偏移量
    int16 level;                  // 层级
    DatumPtr value;               // 向量值
    VamanaNeighborsPtr neighbors; // 邻居列表
    uint32 hash;                  // 哈希值
    LWLock lock;
} VamanaElementData;

typedef VamanaElementData *VamanaElement;

/* Vamana 构建状态 */
typedef struct VamanaBuildState
{

    // 数据集路径
    char *data_path;

    // 构建参数
    int R;        // 图的最大度数（max degree）
    int L;        //  搜索列表大小,建议大于R
    int B;        // 最终索引的RAM限制
    int M;        // 用于构建索引的内存限制
    int T;        // 线程数
    double alpha; // alpha参数

    Relation heap;
    Relation index;
    IndexInfo *indexInfo;

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

/* HNSW index options */
typedef struct VamanaOptions
{
} VamanaOptions;

/* 函数声明 */
void vamanainit(void);
IndexBuildResult *vamanabuild(Relation heap, Relation index,
                              IndexInfo *indexInfo);
void vamanabuildempty(Relation index);
bool vamanainsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique
#if PG_VERSION_NUM >= 140000
                  ,
                  bool indexUnchanged
#endif
                  ,
                  IndexInfo *indexInfo);
IndexBulkDeleteResult *vamanabulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state);
IndexBulkDeleteResult *vamanavacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
IndexScanDesc vamanabeginscan(Relation index, int nkeys, int norderbys);
void vamanarescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                  ScanKey orderbys, int norderbys);
bool vamanagettuple(IndexScanDesc scan, ScanDirection dir);
void vamanaendscan(IndexScanDesc scan);
#endif /* VAMANA_H */