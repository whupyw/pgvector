#include "postgres.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "vamana.h"
#include "miscadmin.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

PG_FUNCTION_INFO_V1(vamanahandler);

/* 代价估算函数 */
static void vamanacostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                               Cost *indexStartupCost, Cost *indexTotalCost,
                               Selectivity *indexSelectivity, double *indexCorrelation,
                               double *indexPages)
{
    // // 简单的代价估算
    // *indexStartupCost = 0;
    // *indexTotalCost = path->indexinfo->tuples * loop_count * cpu_index_tuple_cost;
    // *indexSelectivity = 1.0 / path->indexinfo->tuples;
    // *indexCorrelation = 0;
    // *indexPages = path->indexinfo->pages;
}

/* 索引选项函数 */

static bytea *
vamanaoptions(Datum reloptions, bool validate)
{
    // static const relopt_parse_elt tab[] = {
    //     {"r", RELOPT_TYPE_INT, offsetof(VamanaOptions, r)},
    //     {"l", RELOPT_TYPE_INT, offsetof(VamanaOptions, l)},
    //     {"alpha", RELOPT_TYPE_REAL, offsetof(VamanaOptions, alpha)}};

    // return (bytea *)build_reloptions(reloptions, validate,
    //                                  RELOPT_KIND_VAMANA,
    //                                  sizeof(VamanaOptions),
    //                                  tab, lengthof(tab));
    return NULL;
}

/* 构建阶段名称函数 */
static char *
vamanabuildphasename(int64 phasenum)
{
    switch (phasenum)
    {
    case PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE:
        return "initializing";
    case PROGRESS_VAMANA_PHASE_LOAD:
        return "loading tuples";
    default:
        return NULL;
    }
}

/* 验证函数 */
static bool vamanavalidate(Oid opclassoid)
{
    // 简单返回true,表示总是有效
    return true;
}

/*
 * 定义索引访问方法处理函数
 */
Datum vamanahandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    /* 基本信息 */
    amroutine->amstrategies = 0;
    amroutine->amsupport = 3; /* l2_distance, vector_norm, l2_normalize */
    amroutine->amoptsprocnum = 0;

    /* 功能标志 */
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = true;           /* 支持按操作符排序(KNN搜索) */
    amroutine->amcanbackward = false;           /* 不支持反向扫描 */
    amroutine->amcanunique = false;             /* 不支持唯一索引 */
    amroutine->amcanmulticol = false;           /* 不支持多列索引 */
    amroutine->amoptionalkey = true;            /* 允许跳过索引扫描条件 */
    amroutine->amsearcharray = false;           /* 不支持数组搜索 */
    amroutine->amsearchnulls = false;           /* 不支持NULL搜索 */
    amroutine->amstorage = false;               /* 不需要特殊存储 */
    amroutine->amclusterable = false;           /* 不支持聚簇 */
    amroutine->ampredlocks = false;             /* 不需要细粒度锁 */
    amroutine->amcanparallel = false;           /* 暂不支持并行扫描 */
    amroutine->amcaninclude = false;            /* 不支持INCLUDE列 */
    amroutine->amusemaintenanceworkmem = false; /* 不使用maintenance_work_mem */
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
    amroutine->amkeytype = InvalidOid;

    /* 接口函数 */
    // 索引构建和查找相关函数
    amroutine->ambuild = vamanabuild;
    amroutine->ambuildempty = vamanabuildempty;
    amroutine->amoptions = vamanaoptions;
    amroutine->ambeginscan = vamanabeginscan;
    amroutine->amrescan = vamanarescan;
    amroutine->amgettuple = vamanagettuple;
    amroutine->amendscan = vamanaendscan;


    amroutine->aminsert = vamanainsert;
    amroutine->ambulkdelete = vamanabulkdelete;
    amroutine->amvacuumcleanup = vamanavacuumcleanup;

    /* 扫描相关函数 */
    amroutine->amcanreturn = NULL; /* 不支持索引覆盖扫描 */
    amroutine->amcostestimate = vamanacostestimate;
    
    amroutine->amproperty = NULL; /* 未定义特殊属性 */
    amroutine->ambuildphasename = vamanabuildphasename;
    amroutine->amvalidate = vamanavalidate;

    /* 扫描迭代器函数 */
    
    amroutine->amgetbitmap = NULL; /* 不支持bitmap扫描 */
    
    amroutine->ammarkpos = NULL;  /* 不支持标记位置 */
    amroutine->amrestrpos = NULL; /* 不支持恢复位置 */

    /* 并行扫描相关函数(暂不支持) */
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;

    PG_RETURN_POINTER(amroutine);
}
