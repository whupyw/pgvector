#include "postgres.h"
#include "vamana.h"

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



/* 开始扫描 */
IndexScanDesc
vamanabeginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    // VamanaScanOpaque so;

    // // 创建扫描描述符
    // scan = RelationGetIndexScan(index, nkeys, norderbys);

    // // 分配扫描私有数据
    // so = (VamanaScanOpaque)palloc(sizeof(VamanaScanOpaqueData));
    // so->first = true;
    // so->k = vamana_l; // 使用l参数作为k近邻数

    // scan->opaque = so;

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