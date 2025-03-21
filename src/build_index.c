#include "postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "utils/memutils.h"
#include "diskann.h" // 假设有对应的 C 语言头文件
#include "executor/spi.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "vector.h"
#include <unistd.h>
#include "storage/fd.h"
#include <stdint.h>
#include <errno.h>

#define MAX_PARAM_COUNT 9

#define NUM_PQ_CENTROIDS 256
#define NUM_KMEANS_REPS_PQ 20

// 函数声明
bool file_exists(const char *path);

size_t load_pq_pivots(const char *filename, void **data, size_t *num_centers, size_t *ndims, void **centroid, size_t **chunk_offsets, uint32_t *num_chunks)
{
    elog(INFO, "Reading binary file: %s", filename);
    int fd = OpenTransientFile(filename, O_RDONLY);
    if (fd < 0)
    {
        elog(ERROR, "Could not open file %s for reading", filename);
        return 0;
    }

    uint32_t num_centers_i32, ndims_i32;
    size_t expected_bytes = 0, bytes_read = 0;

    if (read(fd, &num_centers_i32, sizeof(uint32_t)) != sizeof(uint32_t) ||
        read(fd, &ndims_i32, sizeof(uint32_t)) != sizeof(uint32_t))
    {
        elog(ERROR, "Failed to read num_centers or ndims from file %s", filename);
        CloseTransientFile(fd);
        return 0;
    }
    bytes_read += 2 * sizeof(uint32_t);

    *num_centers = (size_t)num_centers_i32;
    *ndims = (size_t)ndims_i32;

    *data = palloc(*num_centers * *ndims * sizeof(float));
    *centroid = palloc(*ndims * sizeof(float));

    expected_bytes = *num_centers * *ndims * sizeof(float) + *ndims * sizeof(float);
    if (read(fd, *data, *num_centers * *ndims * sizeof(float)) != (ssize_t)(*num_centers * *ndims * sizeof(float)) ||
        read(fd, *centroid, *ndims * sizeof(float)) != (ssize_t)(*ndims * sizeof(float)))
    {
        elog(ERROR, "Failed to read data or centroid from file %s", filename);
        CloseTransientFile(fd);
        return 0;
    }
    bytes_read += expected_bytes;

    //*num_chunks = *num_centers; // 这里假设 num_chunks = num_centers
    *chunk_offsets = palloc((*num_chunks + 1) * sizeof(size_t));
    expected_bytes = (*num_chunks + 1) * sizeof(size_t);
    if (read(fd, *chunk_offsets, expected_bytes) != (ssize_t)expected_bytes)
    {
        elog(ERROR, "Failed to read chunk_offsets from file %s", filename);
        CloseTransientFile(fd);
        return 0;
    }
    bytes_read += expected_bytes;

    CloseTransientFile(fd);
    elog(LOG, "Finished reading binary file: %s, total bytes read: %zu", filename, bytes_read);
    return bytes_read;
}

void compute_closest_centers(const float *data, size_t num_points, size_t dim,
                             const float *centers, size_t num_centers, uint32_t *closest_center)
{
    for (size_t i = 0; i < num_points; i++)
    {
        float min_dist = INFINITY;
        uint32_t best_center = 0;

        for (size_t c = 0; c < num_centers; c++)
        {
            float dist = 0.0;
            for (size_t d = 0; d < dim; d++)
            {
                float diff = data[i * dim + d] - centers[c * dim + d];
                dist += diff * diff;
            }

            if (dist < min_dist)
            {
                min_dist = dist;
                best_center = c;
            }
        }
        closest_center[i] = best_center;
    }
}

int load_bin_size_t(const char *file_path, size_t **data, size_t *nr, size_t *nc)
{
    int fd = OpenTransientFile(file_path, O_RDONLY);
    if (fd < 0)
    {
        elog(ERROR, "Could not open file: %s", file_path);
        return -1;
    }

    if (read(fd, nr, sizeof(size_t)) != sizeof(size_t) ||
        read(fd, nc, sizeof(size_t)) != sizeof(size_t))
    {
        elog(ERROR, "Failed to read dimensions from file: %s", file_path);
        CloseTransientFile(fd);
        return -1;
    }

    *data = (size_t *)palloc((*nr) * (*nc) * sizeof(size_t));
    if (read(fd, *data, (*nr) * (*nc) * sizeof(size_t)) != (*nr) * (*nc) * sizeof(size_t))
    {
        elog(ERROR, "Failed to read data from file: %s", file_path);
        pfree(*data);
        CloseTransientFile(fd);
        return -1;
    }

    CloseTransientFile(fd);
    return 0;
}

void load_bin_float(const char *bin_file, float **data, size_t *npts, size_t *dim, size_t offset)
{
    FILE *fp = fopen(bin_file, "rb");
    if (!fp)
    {
        fprintf(stderr, "Error opening file %s: %s\n", bin_file, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (fseek(fp, offset, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error seeking file %s\n", bin_file);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    if (fread(npts, sizeof(size_t), 1, fp) != 1 || fread(dim, sizeof(size_t), 1, fp) != 1)
    {
        fprintf(stderr, "Error reading metadata from file %s\n", bin_file);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    *data = (float *)malloc((*npts) * (*dim) * sizeof(float));
    if (!*data)
    {
        fprintf(stderr, "Memory allocation failed\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    if (fread(*data, sizeof(float), (*npts) * (*dim), fp) != (*npts) * (*dim))
    {
        fprintf(stderr, "Error reading data from file %s\n", bin_file);
        free(*data);
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    fclose(fp);
}

size_t save_pq_pivots(const char *filename, void *data, size_t num_centers, size_t ndims, void *centroid, size_t *chunk_offsets, size_t num_chunks)
{
    elog(INFO, "Writing binary file: %s, num_centers: %zu, ndims: %zu", filename, num_centers, ndims);
    int fd = OpenTransientFile(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        elog(ERROR, "Could not open file %s for writing", filename);
        return 0;
    }

    uint32_t num_centers_i32 = (uint32_t)num_centers, ndims_i32 = (uint32_t)ndims;
    size_t bytes_written = 0;

    bytes_written += write(fd, &num_centers_i32, sizeof(uint32_t));
    bytes_written += write(fd, &ndims_i32, sizeof(uint32_t));
    bytes_written += write(fd, data, num_centers * ndims * sizeof(float));
    bytes_written += write(fd, centroid, ndims * sizeof(float));
    bytes_written += write(fd, chunk_offsets, (num_chunks + 1) * sizeof(size_t));

    CloseTransientFile(fd);
    elog(LOG, "Finished writing binary file: %s", filename);
    return bytes_written;
}

void kmeanspp_selecting_pivots(float *data, size_t num_points, size_t dim, float *pivot_data, size_t num_centers)
{
    if (num_points > (1 << 23))
    {
        elog(ERROR, "ERROR: num_points too large for k-means++, fallback to random selection");
        return;
    }

    size_t *picked = (size_t *)palloc(num_centers * sizeof(size_t));
    float *dist = (float *)palloc(num_points * sizeof(float));
    unsigned int seed = rand();
    size_t init_id = rand_r(&seed) % num_points;
    picked[0] = init_id;
    memcpy(pivot_data, data + init_id * dim, dim * sizeof(float));
    size_t max_try = 10;
#pragma omp parallel for schedule(static, 8192)
    for (size_t i = 0; i < num_points; i++)
    {
        dist[i] = 0.0;
        for (size_t d = 0; d < dim; d++)
        {
            float diff = data[i * dim + d] - data[init_id * dim + d];
            dist[i] += diff * diff;//计算与聚类中心的距离
        }
    }

    size_t num_picked = 1;

    while (num_picked < num_centers)
    {
        double sum = 0.0;
        for (size_t i = 0; i < num_points; i++)
        {
            sum += dist[i];
        }

        double dart_val = ((double)rand_r(&seed) / RAND_MAX) * sum;
        double prefix_sum = 0.0;
        size_t tmp_pivot = 0;

        for (size_t i = 0; i < num_points; i++)
        {
            prefix_sum += dist[i];
            if (dart_val < prefix_sum)
            {
                tmp_pivot = i;
                break;
            }
        }

        // 避免聚类中心重复
        for (size_t j = 0; j < num_picked; j++)
        {
            if (picked[j] == tmp_pivot)
            {
                max_try--;
                if (max_try < 0)
                {
                    elog(ERROR, "ERROR: k-means++ failed to select unique pivots, fallback to random selection");
                    pfree(picked);
                    pfree(dist);
                    return;
                }
                continue;
            }
        }

        picked[num_picked] = tmp_pivot;
        memcpy(pivot_data + num_picked * dim, data + tmp_pivot * dim, dim * sizeof(float));
        num_picked++;
        for (size_t i = 0; i < num_points; i++)
        {
            float new_dist = 0.0;
            for (size_t d = 0; d < dim; d++)
            {
                float diff = data[i * dim + d] - data[tmp_pivot * dim + d];
                new_dist += diff * diff;
            }
            if (new_dist < dist[i])
            {
                dist[i] = new_dist;
            }
        }
    }
    pfree(picked);
    pfree(dist);
}

float run_lloyds(float *data, size_t num_points, size_t dim, float *centers, size_t num_centers, size_t max_reps, uint32_t *closest_center)
{
    float *new_centers = (float *)palloc(num_centers * dim * sizeof(float));
    uint32_t *counts = (uint32_t *)palloc(num_centers * sizeof(uint32_t));
    float residual = 0.0;

    for (size_t iter = 0; iter < max_reps; iter++)
    {
        memset(new_centers, 0, num_centers * dim * sizeof(float));
        memset(counts, 0, num_centers * sizeof(uint32_t));
        residual = 0.0;

        for (size_t i = 0; i < num_points; i++)
        {
            float min_dist = INFINITY;
            uint32_t best_center = 0;
            for (size_t c = 0; c < num_centers; c++)
            {
                float dist = 0.0;
                for (size_t d = 0; d < dim; d++)
                {
                    float diff = data[i * dim + d] - centers[c * dim + d];
                    dist += diff * diff;
                }
                if (dist < min_dist)
                {
                    min_dist = dist;
                    best_center = c;
                }
            }
            closest_center[i] = best_center;
            residual += min_dist;
            for (size_t d = 0; d < dim; d++)
            {
                new_centers[best_center * dim + d] += data[i * dim + d];
            }
            counts[best_center]++;
        }

        for (size_t c = 0; c < num_centers; c++)
        {
            if (counts[c] > 0)
            {
                for (size_t d = 0; d < dim; d++)
                {
                    centers[c * dim + d] = new_centers[c * dim + d] / counts[c];
                }
            }
        }
    }

    pfree(new_centers);
    pfree(counts);
    return residual;
}

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
    snprintf(query, sizeof(query), "SELECT %s FROM %s", column_name, table_name);
    int ret = SPI_exec(query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "SPI_exec failed: %s", query);
        SPI_finish();
        return NULL;
    }

    // 获取数据点数（行数）
    *npts = (size_t)SPI_processed;
    elog(INFO, "npts = %zu", *npts);
    if (*npts == 0)
    {
        elog(WARNING, "No data found in table: %s", table_name);
        SPI_finish();
        return NULL;
    }

    // 读取第一个 vector 以确定维度
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
    // 使用 PostgreSQL 内存管理
    ctx = AllocSetContextCreate(CurrentMemoryContext,
                                "VectorDataMemoryContext",
                                ALLOCSET_DEFAULT_MINSIZE,
                                ALLOCSET_DEFAULT_INITSIZE,
                                ALLOCSET_DEFAULT_MAXSIZE);
    old_ctx = MemoryContextSwitchTo(ctx);

    // 分配内存存储数据
    // elog(INFO, "ready MemoryContextAlloc");
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
        // elog(INFO,"loop:i = %ld", i);
        //  复制数据到 inputdata
        memcpy(inputdata + i * (*ndims), vec_data, (*ndims) * sizeof(float));
    }

    MemoryContextSwitchTo(old_ctx);
    SPI_finish();

    return inputdata;
}

void get_vector_data_param(const char *table_name, const char *column_name, size_t *npts, size_t *ndims)
{
    elog(INFO, "开始获取向量维度与行数");

    // 连接到 SPI
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(ERROR, "SPI_connect 失败");
        return;
    }

    // 阶段 1: 获取总行数
    //---------------------------------------
    char count_query[256];
    snprintf(count_query, sizeof(count_query),
             "SELECT COUNT(*) FROM %s",
             table_name);

    int ret = SPI_exec(count_query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "COUNT(*) 查询失败: %s", count_query);
        SPI_finish();
        return;
    }

    // 从 COUNT(*) 结果中提取行数
    bool isnull;
    Datum rowcount = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
    {
        elog(ERROR, "行数结果为 NULL");
        SPI_finish();
        return;
    }
    *npts = DatumGetInt64(rowcount);
    elog(INFO, "数据点数量 npts = %zu", *npts);

    // 阶段 2: 获取向量维度
    //---------------------------------------
    char sample_query[256];
    snprintf(sample_query, sizeof(sample_query),
             "SELECT %s FROM %s LIMIT 1", // 仅取第一行避免全表扫描
             column_name, table_name);

    ret = SPI_exec(sample_query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "采样查询失败: %s", sample_query);
        SPI_finish();
        return;
    }

    // 检查是否有数据
    if (SPI_processed == 0)
    {
        elog(ERROR, "表 %s 中没有数据", table_name);
        SPI_finish();
        return;
    }

    // 提取第一行的向量维度
    Datum first_val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
    {
        elog(ERROR, "首行向量为 NULL");
        SPI_finish();
        return;
    }
    Vector *vec = (Vector *)DatumGetPointer(first_val);
    *ndims = vec->dim;
    elog(INFO, "向量维度 ndims = %zu", *ndims);

    // 清理 SPI 连接
    SPI_finish();
}

void load_vector_data_to_mem(const char *table_name, const char *column_name, size_t *npts, size_t *ndims,float* data)
{
    elog(INFO, "start load_vector_data");
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(ERROR, "SPI_connect failed");
        //return NULL;
    }

    char query[256];
    snprintf(query, sizeof(query), "SELECT %s FROM %s", column_name, table_name);
    int ret = SPI_exec(query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "SPI_exec failed: %s", query);
        SPI_finish();
        //return NULL;
    }

    // 读取第一个 vector 以确定维度
    bool isnull;
    Datum first_val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
    if (isnull)
    {
        elog(ERROR, "First vector is NULL");
        SPI_finish();
        //return NULL;
    }
    //Vector *vec = (Vector *)DatumGetPointer(first_val);
    //*ndims = vec->dim; // 获取 vector 维度

    elog(INFO, "print npts = %zu, ndims = %zu", *npts, *ndims);

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
        // elog(INFO,"loop:i = %ld", i);
        //  复制数据到 inputdata
        memcpy(data + i * (*ndims), vec_data, (*ndims) * sizeof(float));
    }
    SPI_finish();
}

/* 采样函数 */
void gen_random_slice(const float *inputdata, size_t npts, size_t ndims, double p_val, float **sampled_data, size_t *slice_size)
{
    elog(INFO, "Generating random slice of data with p_val = %.2f", p_val);
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
// void generate_pq_pivots(float *train_data, size_t train_size, uint32_t train_dim,
//                         uint32_t num_centroids, uint32_t num_pq_chunks,
//                         uint32_t num_reps, const char *pivots_path, bool make_zero_mean);
int generate_pq_pivots(const float *train_data, size_t num_train, uint32_t dim, uint32_t num_centers,
                       uint32_t num_pq_chunks, uint32_t max_k_means_reps, const char *pq_pivots_path,
                       bool make_zero_mean)
{
    elog(INFO, "Generating PQ pivots for %zu training points, %u dimension, %u centroids, %u PQ chunks, %u max K-means reps, %s", num_train, dim, num_centers, num_pq_chunks, max_k_means_reps, pq_pivots_path);
    if (num_pq_chunks > dim)
    {
        elog(ERROR, "Number of PQ chunks exceeds dimension");
        return -1;
    }

    MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
                                                  "PQ Pivot Generation Context",
                                                  ALLOCSET_DEFAULT_SIZES);
    MemoryContext oldcontext = MemoryContextSwitchTo(context);

    float *train_data_copy = (float *)palloc(num_train * dim * sizeof(float));
    memcpy(train_data_copy, train_data, num_train * dim * sizeof(float));

    float *full_pivot_data = NULL;

    elog(INFO, "Check if file exists: %s", pq_pivots_path);
    /* Check if file exists */
    if (!file_exists(pq_pivots_path))
    {
        elog(INFO, "PQ pivot file exists. Not generating again");
        MemoryContextSwitchTo(oldcontext);
        MemoryContextDelete(context);
        return -1;
    }

    elog(INFO, "Generating PQ pivots");
    /* Zero-mean normalization */
    float *centroid = (float *)palloc0(dim * sizeof(float));
    if (make_zero_mean)
    {
        for (size_t d = 0; d < dim; d++)
        {
            for (size_t p = 0; p < num_train; p++)
            {
                centroid[d] += train_data_copy[p * dim + d];
            }
            centroid[d] /= num_train;
        }

        for (size_t d = 0; d < dim; d++)
        {
            for (size_t p = 0; p < num_train; p++)
            {
                train_data_copy[p * dim + d] -= centroid[d];
            }
        }
    }

    size_t *chunk_offsets = (size_t *)palloc((num_pq_chunks + 1) * sizeof(size_t));
    size_t low_val = dim / num_pq_chunks;
    size_t high_val = (dim % num_pq_chunks) ? low_val + 1 : low_val;
    size_t num_high = dim - (low_val * num_pq_chunks);

    chunk_offsets[0] = 0;
    for (uint32_t i = 1; i <= num_pq_chunks; i++)
    {
        chunk_offsets[i] = chunk_offsets[i - 1] + ((i <= num_high) ? high_val : low_val);
    }

    full_pivot_data = (float *)palloc(num_centers * dim * sizeof(float));

    for (size_t i = 0; i < num_pq_chunks; i++)
    {
        size_t cur_chunk_size = chunk_offsets[i + 1] - chunk_offsets[i];
        if (cur_chunk_size == 0)
            continue;

        float *cur_pivot_data = (float *)palloc(num_centers * cur_chunk_size * sizeof(float));
        float *cur_data = (float *)palloc(num_train * cur_chunk_size * sizeof(float));
        uint32_t *closest_center = (uint32_t *)palloc(num_train * sizeof(uint32_t));

        elog(LOG, "Processing chunk %zu with dimensions [%zu, %zu)", i, chunk_offsets[i], chunk_offsets[i + 1]);

        for (size_t j = 0; j < num_train; j++)
        {
            memcpy(cur_data + j * cur_chunk_size, train_data_copy + j * dim + chunk_offsets[i],
                   cur_chunk_size * sizeof(float));
        }

        /* Run k-means++ and Lloyd's algorithm (to be implemented) */
        /* 1. 先使用 k-means++ 选择初始中心 */
        kmeanspp_selecting_pivots(cur_data, num_train, cur_chunk_size, cur_pivot_data, num_centers);

        /* 2. 再使用 Lloyd’s 进行迭代优化 */
        run_lloyds(cur_data, num_train, cur_chunk_size, cur_pivot_data, num_centers, max_k_means_reps, closest_center);
        for (size_t j = 0; j < num_centers; j++)
        {
            memcpy(full_pivot_data + j * dim + chunk_offsets[i], cur_pivot_data + j * cur_chunk_size,
                   cur_chunk_size * sizeof(float));
        }

        pfree(cur_pivot_data);
        pfree(cur_data);
        pfree(closest_center);
    }

    /* Save binary data (to be implemented using PostgreSQL file APIs) */
    int ret = save_pq_pivots(pq_pivots_path, full_pivot_data, num_centers, dim, centroid, chunk_offsets, num_pq_chunks);
    elog(INFO, "save_pq_pivots ret = %d", ret);
    pfree(train_data_copy);
    pfree(full_pivot_data);
    pfree(centroid);
    pfree(chunk_offsets);

    MemoryContextSwitchTo(oldcontext);
    MemoryContextDelete(context);

    elog(INFO, "Saved PQ pivot data to %s", pq_pivots_path);
    return 0;
}

// void generate_opq_pivots(float *train_data, size_t train_size, uint32_t train_dim,
//                          uint32_t num_centroids, uint32_t num_pq_chunks,
//                          const char *pivots_path, bool make_zero_mean);

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
    size_t *chunk_offsets;
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
    int ret = load_pq_pivots(pq_pivots_path, &full_pivot_data, &num_centers, &dim, &centroid, &chunk_offsets, &num_pq_chunks);
    elog(INFO, "load_pq_pivots ret = %d", ret);
    if (ret == 0)
    {
        elog(ERROR, "Error loading PQ pivot data from file: %s", pq_pivots_path);
        return -1;
    }

    if (use_opq)
    {
        char rotmat_path[256];
        sprintf(rotmat_path, "%s_rotation_matrix.bin", pq_pivots_path); /* 替换 snprintf */
        load_bin_float(rotmat_path, &rotmat_tr, &nr, &nc, 0);
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
    size_t train_size;
    size_t train_dim = 128;
    float *train_data = NULL;
    size_t npts;
    size_t ndims; // 128维向量
    get_vector_data_param("vectors", "embedding", &npts, &ndims);
    float *inputdata = (float *)palloc(npts * ndims * sizeof(float));
    load_vector_data_to_mem("vectors", "embedding", &npts, &ndims, inputdata);
    float *sampled_data = NULL;
    size_t slice_size = 0;

    elog(INFO, "start generate_quantized_data");

    if (!file_exists(codebook_prefix))
    {
        // 生成随机数据切片
        gen_random_slice(inputdata, npts, ndims, p_val, &sampled_data, &slice_size);
        elog(INFO, "Training data with %zu samples loaded.", slice_size); // 使用 NOTICE 级别

        bool make_zero_mean = true;
        if (compare_metric == DISKANN_INNER_PRODUCT)
            make_zero_mean = false;
        if (use_opq) // OPQ 不需要中心化
            make_zero_mean = false;

        if (!use_opq)
        {
            elog(INFO, "start generate pivots");
            generate_pq_pivots(sampled_data, slice_size, (uint32_t)ndims,
                               NUM_PQ_CENTROIDS, (uint32_t)num_pq_chunks,
                               NUM_KMEANS_REPS_PQ, pq_pivots_path, make_zero_mean);
        }
        else
        {
            // generate_opq_pivots(sampled_data, slice_size, (uint32_t)train_dim,
            //                     NUM_PQ_CENTROIDS, (uint32_t)num_pq_chunks,
            //                     pq_pivots_path, make_zero_mean);
            elog(INFO, "Skip OPQ Training with predefined pivots in: %s", pq_pivots_path); // 使用 LOG 级别
        }
        free(train_data);
    }
    else
    {
        elog(INFO, "Skip Training with predefined pivots in: %s", pq_pivots_path); // 使用 LOG 级别
    }

    // 生成PQ压缩数据
    elog(INFO, "Generating PQ compressed data");
    generate_pq_data_from_pivots(data_file_to_use, NUM_PQ_CENTROIDS,
                                 (uint32_t)num_pq_chunks, pq_pivots_path,
                                 pq_compressed_vectors_path, use_opq);
}

// 辅助函数实现
bool file_exists(const char *path)
{
    elog(INFO, "file_exists:%s", path);
    struct stat buffer;
#if defined(_WIN32)
    return _stat(path, &buffer) == 0; // Windows 使用 _stat
#else
    return stat(path, &buffer) == 0; // Linux/macOS 使用 stat
#endif
}

void create_disk_layout()
{
}

int build_disk_index(const char *dataFilePath, const char *indexFilePath,
                     const char *indexBuildParameters, enum diskann_metric_t compareMetric,
                     int use_opq, const char *codebook_prefix, size_t npt, size_t dim)
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
    int created_temp_file_for_processed_data = 0;
    size_t num_pq_chunks = 8;
    double p_val = 0.1;

    time_t start = time(NULL);
    time_t end = time(NULL);
    // char *pq_pivots_path = indexFilePath + "_pq_pivots.bin";
    const char *compress_suffix = "_pq_compressed_vectors.bin";
    const char *suffix = "_pq_pivots.bin";
    size_t total_len = strlen(indexFilePath) + strlen(suffix) + 1;
    size_t total_len_compress = strlen(indexFilePath) + strlen(compress_suffix) + 1;
    char *pq_pivots_path = palloc(total_len);
    char *pq_compressed_vectors_path = palloc(total_len_compress);
    if (pq_pivots_path == NULL)
    {
        perror("palloc failed");
        return -1;
    }
    strcpy(pq_pivots_path, indexFilePath);
    strcat(pq_pivots_path, suffix);
    strcpy(pq_compressed_vectors_path, indexFilePath);
    strcat(pq_compressed_vectors_path, compress_suffix);
    char *buildParams = strdup(indexBuildParameters);
    /* 解析 indexBuildParameters */
    token = strtok(buildParams, " ");
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
    // ereport(LOG, (errmsg("开始构建索引: R=%u, L=%u, 线程数=%u", R, L, num_threads)));
    elog(INFO, "开始构建索引: R=%u, L=%u, 线程数=%u", R, L, num_threads);

    // 生成量化数据
    generate_quantized_data(dataFilePath, pq_pivots_path, pq_compressed_vectors_path, compareMetric, p_val, num_pq_chunks, use_opq, codebook_prefix);

    /* 清理临时文件（如果有的话） */
    if (created_temp_file_for_processed_data)
    {
        elog(DEBUG1, "删除临时文件");
    }
    pfree(pq_pivots_path);
    pfree(pq_compressed_vectors_path);
    free(buildParams);
    return 0;
}