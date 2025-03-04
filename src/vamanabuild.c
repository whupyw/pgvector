#include "postgres.h"
#include "vamana.h"

/* Vamana图构建 */
static void
BuildVamanaGraph(VamanaBuildState *buildstate)
{
    // 初始化构建参数
    int r = buildstate->r;
    int l = buildstate->l;
    double alpha = buildstate->alpha;

    // 分配内存
    MemoryContext oldCtx = MemoryContextSwitchTo(buildstate->graphCtx);

    // 并行构建支持
    if (buildstate->isParallel)
    {
        // 初始化共享内存
        InitializeSharedMemory(buildstate);

        // 启动工作进程
        LaunchParallelWorkers(buildstate);
    }

    // 构建图结构
    for (int level = VAMANA_MAX_LEVEL - 1; level >= 0; level--)
    {
        // 对每一层执行:

        // 1. 随机选择入口点
        VamanaElement entryPoint = SelectRandomEntryPoint(level);

        // 2. 对每个元素执行:
        while (HasMoreElements())
        {
            VamanaElement element = GetNextElement();

            // 2.1 搜索最近邻
            List *neighbors = SearchNeighbors(element, entryPoint, l);

            // 2.2 选择候选集
            List *candidates = SelectCandidates(neighbors, r, alpha);

            // 2.3 建立连接
            ConnectElements(element, candidates);
        }
    }

    // 清理
    MemoryContextSwitchTo(oldCtx);
}

/* 构建索引入口函数 */
IndexBuildResult *
VamanaBuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    VamanaBuildState buildstate;

    // 初始化构建状态
    InitBuildState(&buildstate, heap, index, indexInfo);

    // 构建图结构
    BuildVamanaGraph(&buildstate);

    // 返回构建结果
    return CreateBuildResult(&buildstate);
}