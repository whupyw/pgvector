#include "postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "utils/memutils.h"
#include "diskann.h" // 假设有对应的 C 语言头文件
#include "executor/spi.h"
#include "utils/memutils.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "vector.h"

#define MAX_PARAM_COUNT 9

#define NUM_PQ_CENTROIDS 256
#define NUM_KMEANS_REPS_PQ 20

// 函数声明
bool file_exists(const char *path);

// void gen_random_slice(const char *data_file, double p_val, float **train_data, size_t *train_size, size_t *train_dim);

const float *load_vector_data(const char *table_name, const char *column_name, size_t *npts, size_t *ndims)
{
    elog(INFO, "print npts = %zu, ndims = %zu", *npts, *ndims);
    elog(INFO, "start load_vector_data");
    MemoryContext ctx, old_ctx;
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(ERROR, "SPI_connect failed");
        return NULL;
    }

    char query[256];
    snprintf(query, sizeof(query), "SELECT %s FROM %s where id < 4", column_name, table_name);
    elog(INFO, "Executing SQL: %s", query);
    elog(INFO, "start query");
    int ret = SPI_exec(query, 0);
    elog(INFO, "ret = %d", ret);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "SPI_exec failed: %s", query);
        SPI_finish();
        return NULL;
    }

    // 获取数据点数（行数）
    *npts = (size_t)SPI_processed;
    if (*npts == 0)
    {
        elog(WARNING, "No data found in table: %s", table_name);
        SPI_finish();
        return NULL;
    }

    // 读取第一个 vector 以确定维度
    elog(INFO, "ready SPI_getbinval");
    bool isnull;
    Datum first_val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
    {
        elog(ERROR, "First vector is NULL");
        SPI_finish();
        return NULL;
    }
    elog(INFO, "first vector OK");
    elog(INFO, "ready DatumGetArrayTypeP");
    Vector *vec = (Vector *)DatumGetPointer(first_val);
    *ndims = vec->dim; // 获取 vector 维度
    elog(INFO, "vector OK");
    // 使用 PostgreSQL 内存管理
    ctx = AllocSetContextCreate(CurrentMemoryContext,
                                "VectorDataMemoryContext",
                                ALLOCSET_DEFAULT_MINSIZE,
                                ALLOCSET_DEFAULT_INITSIZE,
                                ALLOCSET_DEFAULT_MAXSIZE);
    old_ctx = MemoryContextSwitchTo(ctx);

    // 分配内存存储数据
    //elog(INFO, "ready MemoryContextAlloc");
    elog(INFO, "print npts = %zu, ndims = %zu", *npts, *ndims);
    float *inputdata = (float *)MemoryContextAlloc(ctx, (*npts) * (*ndims) * sizeof(float));

    // 解析每一行数据
    elog(INFO, "ready for loop");
    for (size_t i = 0; i < *npts; i++)
    {
        HeapTuple tuple = SPI_tuptable->vals[i];
        Datum val = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (isnull)
        {
            elog(ERROR, "NULL vector at row %zu", i);
            continue;
        }

        Vector *vec = (Vector *)DatumGetPointer(val);
        float *vec_data = vec->x;
        elog(INFO, "loop：i = %d", i);
        // 复制数据到 inputdata
        memcpy(inputdata + i * (*ndims), vec_data, (*ndims) * sizeof(float));
    }

    MemoryContextSwitchTo(old_ctx);
    SPI_finish();

    return inputdata;
}

/* 采样函数 */
void gen_random_slice(const float *inputdata, size_t npts, size_t ndims, double p_val, float **sampled_data, size_t *slice_size)
{
    if (p_val > 1.0)
        p_val = 1.0;

    MemoryContext oldContext;
    MemoryContext ctx;

    // 创建一个专属的内存上下文
    ctx = AllocSetContextCreate(CurrentMemoryContext, "SampleDataContext", ALLOCSET_DEFAULT_SIZES);
    oldContext = MemoryContextSwitchTo(ctx);

    float *temp_data = (float *)MemoryContextAlloc(ctx, npts * ndims * sizeof(float));
    if (!temp_data)
    {
        elog(ERROR, "Memory allocation failed for sampled data");
    }

    size_t count = 0;
    srand((unsigned int)time(NULL)); // 初始化随机数种子

    for (size_t i = 0; i < npts; i++)
    {
        if ((double)rand() / RAND_MAX < p_val)
        {
            for (size_t d = 0; d < ndims; d++)
            {
                temp_data[count * ndims + d] = inputdata[i * ndims + d];
            }
            count++;
        }
    }

    // 精确调整内存分配
    *sampled_data = (float *)MemoryContextAlloc(ctx, count * ndims * sizeof(float));
    if (!*sampled_data)
    {
        elog(ERROR, "Memory allocation failed for final sampled data");
    }

    memcpy(*sampled_data, temp_data, count * ndims * sizeof(float));
    *slice_size = count;

    // 释放临时数据
    pfree(temp_data);

    elog(INFO, "Successfully sampled %zu data points (%.2f%% of input)", count, (p_val * 100));
}
void generate_pq_pivots(float *train_data, size_t train_size, uint32_t train_dim,
                        uint32_t num_centroids, uint32_t num_pq_chunks,
                        uint32_t num_reps, const char *pivots_path, bool make_zero_mean);
void generate_opq_pivots(float *train_data, size_t train_size, uint32_t train_dim,
                         uint32_t num_centroids, uint32_t num_pq_chunks,
                         const char *pivots_path, bool make_zero_mean);

int generate_pq_data_from_pivots(const char *data_file, uint32_t num_centers, uint32_t num_pq_chunks,
                                 const char *pq_pivots_path, const char *pq_compressed_vectors_path,
                                 int use_opq) /* 替换 bool 为 int */
{
    size_t read_blk_size;
    FILE *base_reader;
    uint32_t npts32;
    uint32_t basedim32;
    size_t num_points;
    size_t dim;
    MemoryContext ctx;
    MemoryContext old_ctx;
    float *full_pivot_data;
    float *rotmat_tr;
    float *centroid;
    uint32_t *chunk_offsets;
    size_t nr, nc;
    size_t *file_offset_data;
    FILE *compressed_file_writer;
    size_t block_size;
    uint32_t *block_compressed_base;
    float *block_data_tmp;
    size_t num_blocks;
    size_t block;

    read_blk_size = 64 * 1024 * 1024;
    base_reader = fopen(data_file, "rb");
    if (!base_reader)
    {
        elog(ERROR, "Could not open data file: %s", data_file);
        return -1;
    }

    fread(&npts32, sizeof(uint32_t), 1, base_reader);
    fread(&basedim32, sizeof(uint32_t), 1, base_reader);
    num_points = (size_t)npts32;
    dim = (size_t)basedim32;

    ctx = AllocSetContextCreate(CurrentMemoryContext,
                                "PQDataMemoryContext",
                                ALLOCSET_DEFAULT_MINSIZE,
                                ALLOCSET_DEFAULT_INITSIZE,
                                ALLOCSET_DEFAULT_MAXSIZE);
    old_ctx = MemoryContextSwitchTo(ctx);

    full_pivot_data = NULL;
    rotmat_tr = NULL;
    centroid = NULL;
    chunk_offsets = NULL;
    file_offset_data = NULL;

    if (!file_exists(pq_pivots_path))
    {
        elog(ERROR, "PQ k-means pivot file not found: %s", pq_pivots_path);
        return -1;
    }

    /* Load pivot data */
    load_bin_size_t(pq_pivots_path, &file_offset_data, &nr, &nc, 0);
    if (nr != 4)
    {
        elog(ERROR, "Error reading pq_pivots file: %s", pq_pivots_path);
        return -1;
    }

    load_bin_float(pq_pivots_path, &full_pivot_data, &nr, &nc, file_offset_data[0]);
    load_bin_float(pq_pivots_path, &centroid, &nr, &nc, file_offset_data[1]);
    load_bin_uint32(pq_pivots_path, &chunk_offsets, &nr, &nc, file_offset_data[2]);

    if (use_opq)
    {
        char rotmat_path[256];
        sprintf(rotmat_path, "%s_rotation_matrix.bin", pq_pivots_path); /* 替换 snprintf */
        load_bin_float(rotmat_path, &rotmat_tr, &nr, &nc);
    }

    elog(LOG, "Loaded PQ pivot information");

    compressed_file_writer = fopen(pq_compressed_vectors_path, "wb");
    if (!compressed_file_writer)
    {
        elog(ERROR, "Could not open output file: %s", pq_compressed_vectors_path);
        return -1;
    }

    fwrite(&num_points, sizeof(uint32_t), 1, compressed_file_writer);
    fwrite(&num_pq_chunks, sizeof(uint32_t), 1, compressed_file_writer);

    block_size = (num_points <= 8192) ? num_points : 8192;
    block_compressed_base = (uint32_t *)palloc0(block_size * num_pq_chunks * sizeof(uint32_t));
    block_data_tmp = (float *)palloc0(block_size * dim * sizeof(float));

    num_blocks = (num_points + block_size - 1) / block_size;
    for (block = 0; block < num_blocks; block++)
    {
        size_t start_id = block * block_size;
        size_t end_id = (start_id + block_size > num_points) ? num_points : (start_id + block_size);
        size_t cur_blk_size = end_id - start_id;
        size_t i, j;

        fread(block_data_tmp, sizeof(float), cur_blk_size * dim, base_reader);

        for (i = 0; i < num_pq_chunks; i++)
        {
            size_t cur_chunk_size = chunk_offsets[i + 1] - chunk_offsets[i];
            uint32_t *closest_center = (uint32_t *)palloc(cur_blk_size * sizeof(uint32_t));

            compute_closest_centers(block_data_tmp, cur_blk_size, cur_chunk_size, full_pivot_data, num_centers, closest_center);
            for (j = 0; j < cur_blk_size; j++)
            {
                block_compressed_base[j * num_pq_chunks + i] = closest_center[j];
            }
            pfree(closest_center);
        }

        fwrite(block_compressed_base, sizeof(uint32_t), cur_blk_size * num_pq_chunks, compressed_file_writer);
        elog(LOG, "Processed points [%zu, %zu]", start_id, end_id);
    }

    fclose(compressed_file_writer);
    fclose(base_reader);

    MemoryContextSwitchTo(old_ctx);
    MemoryContextDelete(ctx);

    return 0;
}

// 主函数实现
void generate_quantized_data(
    const char *data_file_to_use,
    const char *pq_pivots_path,
    const char *pq_compressed_vectors_path,
    diskann_metric_t compare_metric,
    double p_val,
    size_t num_pq_chunks,
    bool use_opq,
    const char *codebook_prefix)
{
    size_t train_size, train_dim;
    float *train_data = NULL;
    size_t npts = 1000; // 1000个点
    size_t ndims = 128; // 128维向量

    float *inputdata = (float *)palloc(npts * ndims * sizeof(float));
    float *sampled_data = NULL;
    size_t slice_size = 0;

    if (!file_exists(codebook_prefix))
    {
        // 生成随机数据切片
        gen_random_slice(inputdata, npts, ndims, 0.5, &sampled_data, &slice_size);
        elog(NOTICE, "Training data with %zu samples loaded.", train_size); // 使用 NOTICE 级别

        bool make_zero_mean = true;
        if (compare_metric == DISKANN_INNER_PRODUCT)
            make_zero_mean = false;
        if (use_opq) // OPQ 不需要中心化
            make_zero_mean = false;

        if (!use_opq)
        {
            generate_pq_pivots(train_data, train_size, (uint32_t)train_dim,
                               NUM_PQ_CENTROIDS, (uint32_t)num_pq_chunks,
                               NUM_KMEANS_REPS_PQ, pq_pivots_path, make_zero_mean);
        }
        else
        {
            generate_opq_pivots(train_data, train_size, (uint32_t)train_dim,
                                NUM_PQ_CENTROIDS, (uint32_t)num_pq_chunks,
                                pq_pivots_path, make_zero_mean);
        }
        free(train_data);
    }
    else
    {
        elog(INFO, "Skip Training with predefined pivots in: %s", pq_pivots_path); // 使用 LOG 级别
    }

    // 生成PQ压缩数据
    generate_pq_data_from_pivots(data_file_to_use, NUM_PQ_CENTROIDS,
                                 (uint32_t)num_pq_chunks, pq_pivots_path,
                                 pq_compressed_vectors_path, use_opq);
}

// 辅助函数实现
bool file_exists(const char *path)
{
    return true;
}

void build_merged_vamana_index()
{
}

void create_disk_layout()
{
}

int build_disk_index(const char *dataFilePath, const char *indexFilePath,
                     const char *indexBuildParameters, enum diskann_metric_t compareMetric,
                     int use_opq, const char *codebook_prefix)
{

    /* 变量定义部分 */
    char *param_list[10]; /* 存储参数 */
    int param_count = 0;
    char *token;
    unsigned int R, L;
    double final_index_ram_limit, indexing_ram_budget;
    unsigned int num_threads;
    int use_disk_pq = 0;
    unsigned int disk_pq_dims = 0;
    unsigned int build_pq_bytes = 0;
    int reorder_data = 0;
    size_t points_num, dim;
    int created_temp_file_for_processed_data = 0;
    elog(INFO, "start build_index");
    elog(INFO, "debug line_number_118:%s", indexBuildParameters);
    /* 解析 indexBuildParameters */
    token = strtok((char *)indexBuildParameters, " ");
    elog(INFO, "debug line_number_119:");
    while (token != NULL && param_count < 10)
    {
        param_list[param_count++] = token;
        token = strtok(NULL, " ");
    }
    elog(INFO, "debug line_number_120:");
    if (param_count < 5 || param_count > 9)
    {
        ereport(ERROR, (errmsg("参数格式错误: 参数数量 %d 超出范围 (5-9)", param_count)));
        return -1;
    }
    elog(INFO, "debug line_number_126:");
    /* 解析 R、L 等参数 */
    R = (unsigned int)atoi(param_list[0]);
    L = (unsigned int)atoi(param_list[1]);
    final_index_ram_limit = atof(param_list[2]);
    indexing_ram_budget = atof(param_list[3]);
    num_threads = (unsigned int)atoi(param_list[4]);

    if (final_index_ram_limit <= 0 || indexing_ram_budget <= 0)
    {
        ereport(ERROR, (errmsg("内存预算错误: final_index_ram_limit=%.2f, indexing_ram_budget=%.2f",
                               final_index_ram_limit, indexing_ram_budget)));
        return -1;
    }

    /* 线程设置 */
    if (num_threads != 0)
    {
        // omp_set_num_threads(num_threads);
    }

    elog(INFO, "参数解析完成: R=%u, L=%u, final_index_ram_limit=%.2f, indexing_ram_budget=%.2f, 线程数=%u",
         R, L, final_index_ram_limit, indexing_ram_budget, num_threads);

    time_t start = time(NULL);
    time_t end = time(NULL);

    // points_num 向量规模
    // dim 向量维度

    /* 处理 PQ 相关参数 */
    if (param_count > 5)
    {
        disk_pq_dims = atoi(param_list[5]);
        if (disk_pq_dims > 0)
        {
            use_disk_pq = 1;
        }
    }

    if (param_count >= 7)
    {
        reorder_data = atoi(param_list[6]);
    }

    if (param_count >= 8)
    {
        build_pq_bytes = atoi(param_list[7]);
    }

    /* 处理数据文件 */
    if (compareMetric == DISKANN_INNER_PRODUCT)
    {
        elog(INFO, "处理 INNER_PRODUCT 数据");
        created_temp_file_for_processed_data = 1;
    }
    else if (compareMetric == DISKANN_COSINE)
    {
        elog(INFO, "处理 COSINE 数据");
        created_temp_file_for_processed_data = 1;
    }

    /* 构建索引 */
    ereport(LOG, (errmsg("开始构建索引: R=%u, L=%u, 线程数=%u", R, L, num_threads)));

    /* 清理临时文件（如果有的话） */
    if (created_temp_file_for_processed_data)
    {
        elog(DEBUG1, "删除临时文件");
    }

    return 0;
}