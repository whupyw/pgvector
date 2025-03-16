#include "diskann.h"
#include <stdbool.h>
int main()
{
    //print("hello world");
    size_t npt_val = 5;
    // 维度
    size_t dim_val = 5;
    // 切片大小
    size_t slice_size = 0;
    // 加载向量数据
    float *storage = NULL;
    const float *aa = load_vector_data("vectors", "embedding", &npt_val, &dim_val);
    // gen_random_slice(aa, npt_val, dim_val, 0.01, &storage, &slice_size);
    const char *dataFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/siftsmall_learn.fbin";
    const char *indexFile = "/mnt/c/dev/repository/graduation/my_pgvector/pgvector/data/test";
    // L=50,R=64,C=200
    const char *buildParams = "50 64 200 1 1";
    enum diskann_metric_t metric = DISKANN_L2; // 假设使用 L2 作为度量方式
    int use_opq = false;                       // 启用 OPQ
    const char *codebookPrefix = "/path/to/codebook";

    int status = build_disk_index(dataFile, indexFile, buildParams, metric, use_opq, codebookPrefix);
    return 0;
}