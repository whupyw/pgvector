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
#include "pgstat.h"

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
                                       0, 8 * 1024, 256 * 1024*1024);

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
    // VamanaScanOpaque so = (VamanaScanOpaque)scan->opaque;

    // // 释放资源
    // if (so->tmpCtx)
    //     MemoryContextDelete(so->tmpCtx);

    // pfree(so);
    // scan->opaque = NULL;
}
