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
#include "new_vector.h"
#include "vector_cache.h"
#include "neighbour.h"
/* Vamana 特定的参数 */
#define VAMANA_MAX_LEVEL 64      // 最大层数
#define VAMANA_DEFAULT_R 64      // 默认候选集大小
#define VAMANA_DEFAULT_L 100     // 默认搜索列表大小
#define VAMANA_DEFAULT_ALPHA 1.2 // 默认alpha参数

#define VAMANA_MAGIC_NUMBER 0xA953A953 // 文件魔数校验
#define VAMANA_VERSION 1               // 版本
#define VAMANA_PAGE_ID 0xFF91          // 页面类型标识

#define VAMANA_MAX_DIM 1000 // 向量最大维度

#define VAMANA_TYPE_INFO_PROC 3

#define PROGRESS_VAMANA_PHASE_LOAD 2

#define VamanaPageGetOpaque(page) ((VamanaPageOpaque)PageGetSpecialPointer(page))
#define VamanaPageGetMeta(page) ((VamanaMetaPageData *)PageGetContents(page))

// #define VamanaPtrDeclare(type, relptrtype, ptrtype) \
//     relptr_declare(type, relptrtype);               \
//     typedef union                                   \
//     {                                               \
//         type *ptr;                                  \
//         relptrtype relptr;                          \
//     } ptrtype

/* Pointers that can be absolute or relative */
/* Use char for DatumPtr so works with Pointer */
// VamanaPtrDeclare(VamanaElementData, VamanaElementRelptr, VamanaElementPtr);
// VamanaPtrDeclare(VamanaNeighborArray, VamanaNeighborArrayRelptr, VamanaNeighborArrayPtr);
// VamanaPtrDeclare(VamanaNeighborArrayPtr, VamanaNeighborsRelptr, VamanaNeighborsPtr);
// VamanaPtrDeclare(char, DatumRelptr, DatumPtr);

// 简化后的数据结构
typedef struct
{
    size_t capacity;
    size_t size;
    uint32_t *data;
} UIntArray;

typedef struct VamanaPageOpaqueData
{
    BlockNumber nextblkno;
    uint16 unused;
    uint16 page_id; /* for identification of Vamana indexes */
} VamanaPageOpaqueData;

typedef VamanaPageOpaqueData *VamanaPageOpaque;

// 构建索引所需要的数据
typedef struct
{
    // 数据集路径
    char *data_path;

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

typedef struct VamanaMetaPageData
{
    uint32 magicNumber;
    uint32 version;

    // 图结构参数
    uint32 dimensions;
    uint32 R;
    uint32 L;
    char * table_name;
    uint32 num_points;
    uint32 entry_point;
    uint16 m;

    uint16 efConstruction;
    BlockNumber entryBlkno;
    OffsetNumber entryOffno;
    int16 entryLevel;
    BlockNumber insertPage;
} VamanaMetaPageData;

typedef VamanaMetaPageData *VamanaMetaPage;

typedef struct VamanaAllocator
{
    void *(*alloc)(Size size, void *state);
    void *state;
} VamanaAllocator;

typedef struct VamanaGraph
{
    /* Graph state */
    slock_t lock;

    double indtuples;

    /* Entry state */
    LWLock entryLock;
    LWLock entryWaitLock;

    /* Allocations state */
    LWLock allocatorLock;
    Size memoryUsed;
    Size memoryTotal;

    /* Flushed state */
    LWLock flushLock;
    bool flushed;
} VamanaGraph;

/* Vamana 构建状态 */
typedef struct VamanaBuildState
{

    // // 数据集路径
    // char *data_path;

    // 构建参数
    int dimensions;
    int R;        // 图的最大度数（max degree）
    int L;        //  搜索列表大小,建议大于R
    int B;        // 最终索引的RAM限制
    int M;        // 用于构建索引的内存限制
    int T;        // 线程数
    double alpha; // alpha参数

    char *index_table_name;
    char *data_table_name;

    Relation heap;
    Relation index;
    IndexInfo *indexInfo;
    ForkNumber forkNum;
    // 内存管理
    MemoryContext tmpCtx;
    MemoryContext graphCtx;
    VamanaAllocator allocator;

    //
    VamanaGraph graphData;
    VamanaGraph *graph;

    // 并行构建支持
    bool isParallel;
    int workers;

    // 统计信息
    double reltuples;
    double indtuples;
} VamanaBuildState;

typedef struct VamanaOptions
{
    int r;
    int l;
} VamanaOptions;

typedef union
{
    struct pointerhash_hash *pointers;
    struct offsethash_hash *offsets;
    struct tidhash_hash *tids;
} my_visited_hash;

typedef struct VamanaTypeInfo
{
    int maxDimensions;
    Datum (*normalize)(PG_FUNCTION_ARGS);
    void (*checkValue)(Pointer v);
} VamanaTypeInfo;

typedef struct VamanaQuery
{
    Datum value;
} VamanaQuery;

typedef struct VamanaScanOpaqueData
{
    const VamanaTypeInfo *typeInfo;
    bool first;
    List *w;
    //my_visited_hash v;
    //pairingheap *discarded;
    //VamanaQuery q;
    int m;
    int64 tuples;
    double previousDistance;
    Size maxMemory;
    NewVector *heaptids_vectors;
    uint32_t current_idx;
    MemoryContext tmpCtx;

    /* Support functions */
    // VamanaSupport support;
} VamanaScanOpaqueData;

typedef VamanaScanOpaqueData *VamanaScanOpaque;

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
void VamanaInit(void);
Buffer VamanaNewBuffer(Relation index, ForkNumber forkNum);
bool create_index_table(char *index_name, char *table_name, int dimensions);
void get_vectors_and_neighbors(char *index_table_name, NewVector *re_vectors, NewVector *target_vectors);
NewVector* search_k_nearest_neighbors(char *index_table_name, uint32_t init_id,
                                     int k, Vector *target, uint32_t vector_num);
NewVector *new_search_k_nearest_neighbors(char *index_table_name, NewVector *init_ids,
                                      int k, Vector *target, uint32_t vector_num);
FmgrInfo *VamanaOptionalProcInfo(Relation index, uint16 procnum);
const VamanaTypeInfo *VamanaGetTypeInfo(Relation index);
void MyPrintVector(Vector *vector);
void new_vector_to_string(NewVector *vec, char *out, size_t out_size);
size_t calculate_search_entry(size_t num_points);
#endif /* VAMANA_H */