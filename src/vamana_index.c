#include "vamana_index.h"
#include "diskann.h"
#include "my_vector.h"
#include "scratch.h"
#include <stdlib.h>
#include <float.h>
#include <assert.h>
#include "new_vector.h"
#include <math.h>
#include "executor/spi.h"
#include "lib/stringinfo.h"
#define GRAPH_SLACK_FACTOR 1.3f
typedef struct
{
    uint32_t *location_to_labels;
    uint32_t *label_to_start_id;
    uint32_t *aligned_query;
    uint32_t max_points;
    uint32_t num_frozen_pts;
    uint32_t *graph_store;
} DataStore;

const uint32_t *static_compressed_data = NULL;
const float *static_pivot_data = NULL;
const uint32_t *static_neighbors = NULL;
NewVector *static_neighbors_vectors = NULL;
float *static_vector_data = NULL;
uint32_t pq_chunk = 16;

// 获取指定位置向量的邻居索引，存入数组中
void get_neighbors(uint32_t point_index, uint32_t R, uint32_t *neighbors, uint32_t *result_neighbors)
{
    // 检查索引是否有效
    if (point_index >= R)
    {
        printf("错误：给定的点索引超出了邻居数组范围。\n");
        return;
    }

    // 将指定点的邻居（从 neighbors 数组中提取）存入 result_neighbors 数组中
    for (int i = 0; i < R; i++)
    {
        result_neighbors[i] = neighbors[point_index * R + i];
    }
}

void get_neighbors_pointer(uint32_t point_index, uint32_t R, uint32_t *neighbors, uint32_t **result_neighbors)
{
    // 检查索引是否有效
    if (point_index >= R)
    {
        printf("错误：给定的点索引超出了邻居数组范围。\n");
        return;
    }

    *result_neighbors = neighbors + point_index * R;
}

// 交换两个整数
void swap(int *a, int *b)
{
    int temp = *a;
    *a = *b;
    *b = temp;
}

// 为每个向量生成 R 个唯一的随机邻居（不包含自身）
void generate_random_neighbors(uint32_t num_points, uint32_t R, uint32_t *neighbors)
{
    if (R >= num_points)
    {
        printf("R 必须小于 num_points。\n");
        return;
    }

    srand((uint32_t)time(NULL)); // 初始化随机种子

#pragma omp parallel for
    for (int i = 0; i < num_points; i++)
    {
        char *used = (char *)calloc(num_points, sizeof(char)); // 标记已选择的邻居
        if (!used)
        {
            printf("内存分配失败。\n");
            continue;
        }

        used[i] = 1; // 自身不能作为邻居

        int count = 0;
        while (count < R)
        {
            uint32_t rand_idx = rand() % num_points;

            if (!used[rand_idx])
            {
                neighbors[i * R + count] = rand_idx;
                used[rand_idx] = 1;
                count++;
            }
        }

        free(used);
    }
}

// 为每个向量生成 R 个唯一的随机邻居（不包含自身）
bool generate_random_neighbors_for_vector(NewVector *vec, uint32_t num_points, uint32_t R)
{
    if (R >= num_points)
    {
        printf("R 必须小于 num_points。\n");
        return false;
    }
    new_vector_init(vec, sizeof(NewVector)); // 初始化二维数组，元素是行

    srand((uint32_t)time(NULL)); // 初始化随机种子
    for (size_t i = 0; i < num_points; i++)
    {
        NewVector *row = (NewVector *)palloc(sizeof(NewVector));  // 创建每一行
        new_vector_init_with_capacity(row, sizeof(uint32_t), 60); // 每行是一个NewVector，元素类型是Element
        // 将行添加到矩阵中
        new_vector_push_back(vec, row);
        // new_vector_free(row);
    }

#pragma omp parallel for
    for (int i = 0; i < num_points; i++)
    {
        char *used = (char *)calloc(num_points, sizeof(char)); // 标记已选择的邻居
        if (!used)
        {
            printf("内存分配失败。\n");
            continue;
        }

        used[i] = 1; // 自身不能作为邻居

        int count = 0;
        NewVector *cur_vec = (NewVector *)new_vector_get(vec, i);
        while (count < R)
        {
            uint32_t rand_idx = rand() % num_points;

            if (!used[rand_idx])
            {
                // neighbors[i * R + count] = rand_idx;
                // my2d_vector_set(vec, i, count, rand_idx);

                new_vector_push_back(cur_vec, &rand_idx);
                used[rand_idx] = 1;
                count++;
            }
        }

        free(used);
    }
    return true;
}

bool new_save_neighbors_to_disk(NewVector *neighbors, const char *table_name)
{
    // 连接到 PostgreSQL 内部 SPI 上下文
    if (SPI_connect() != SPI_OK_CONNECT)
    {
        elog(ERROR, "SPI_connect failed");
        return false;
    }

    char select_query[256];
    snprintf(select_query, sizeof(select_query), "SELECT vector_id FROM %s", table_name);
    int ret = SPI_exec(select_query, 0);
    if (ret != SPI_OK_SELECT)
    {
        elog(ERROR, "SPI_exec failed: %s", select_query);
        SPI_finish();
        return false;
    }

    size_t npts = (size_t)SPI_processed;
    elog(INFO, "Number of records: %zu", npts);
    if (npts == 0)
    {
        elog(WARNING, "No records found in table vectors.");
        SPI_finish();
        return false;
    }
    // 记录所有id 1->numpoint
    MyVector *ids = (MyVector *)palloc(sizeof(MyVector));
    vector_init(ids);
    for (size_t i = 0; i < npts; i++)
    {
        bool isnull = false;
        HeapTuple tuple = SPI_tuptable->vals[i];
        Datum val = SPI_getbinval(tuple, SPI_tuptable->tupdesc, 1, &isnull);
        if (isnull)
            continue;
        uint32_t id = DatumGetUInt32(val);
        // 记录id
        vector_push_back(ids, id);
    }
    elog(INFO, "记录vector_id");
    // 遍历每一条记录，根据 `id` 更新邻居
    for (size_t i = 0; i < npts; i++)
    {
        // 获取当前记录的 `id` id = 1 i = 0
        uint32_t id = vector_get(ids, i);
        // 假设二维数组 `neighbors_array` 中每行数据对应于一个记录的邻居
        // 构建邻居数组字符串
        StringInfoData array_string;
        initStringInfo(&array_string); // 动态字符串，自动扩容

        // snprintf(array_string, sizeof(array_string), "%d,%d,%d", neighbors_array[i][0], neighbors_array[i][1], neighbors_array[i][2]);
        NewVector *cur_node_neighbors = new_vector_get(neighbors, i);
        for (size_t j = 0; j < cur_node_neighbors->size; j++)
        {
            uint32_t *cur_node_neighbor = new_vector_get(cur_node_neighbors, j);
            if (j > 0)
                appendStringInfoString(&array_string, ",");
            appendStringInfo(&array_string, "%u", *cur_node_neighbor);
        }
        StringInfoData sql;
        initStringInfo(&sql);
        appendStringInfo(&sql,
                         "UPDATE %s SET neighbors = ARRAY[%s] WHERE vector_id = %u",
                         table_name, array_string.data, id);
        // elog(INFO, "Executing query: %s", sql.data);

        // 执行更新操作
        ret = SPI_exec(sql.data, 1); // 更新 1 行
        if (ret != SPI_OK_UPDATE)
        {
            elog(ERROR, "Failed to update neighbor for id %d", id);
            SPI_finish();
            return false;
        }
    }

    // 关闭 SPI 连接
    SPI_finish();
    vector_free(ids);
    pfree(ids);
    return true;
}

// 为每个向量生成 R 个唯一的随机邻居（不包含自身）
bool generate_random_neighbors_for_vector_empty(NewVector *vec, size_t num_points, size_t R)
{
    if (R >= num_points)
    {
        printf("R 必须小于 num_points。\n");
        return false;
    }
    new_vector_init_with_capacity(vec, sizeof(NewVector), num_points); // 初始化二维数组，元素是行

    for (size_t i = 0; i < num_points; i++)
    {
        NewVector row;                                                   // 创建每一行
        new_vector_init_with_capacity(&row, sizeof(uint32_t), (R + 20)); // 每行是一个NewVector，元素类型是Element
        // 将行添加到矩阵中
        new_vector_push_back(vec, &row);
    }
    return true;
}

// 计算搜索起始点的函数
size_t calculate_entry(size_t num_points)
{
    // 生成一个大范围的随机数 r
    size_t r = (size_t)rand() * (size_t)RAND_MAX + (size_t)rand();

    // 返回随机索引，确保它在容量范围内
    return (uint32_t)(r % num_points);
}

int load_compressed_vectors(const char *compressed_file_path, size_t *num_points, uint32_t *num_pq_chunks, uint32_t **compressed_data)
{
    FILE *compressed_file_reader;
    uint32_t npts32, num_pq_chunks32;
    size_t num_blocks, block_size;
    size_t block;

    // 打开压缩文件
    compressed_file_reader = fopen(compressed_file_path, "rb");
    if (!compressed_file_reader)
    {
        fprintf(stderr, "Could not open compressed vectors file: %s\n", compressed_file_path);
        return -1;
    }

    // 读取元数据
    fread(&npts32, sizeof(uint32_t), 1, compressed_file_reader);
    fread(&num_pq_chunks32, sizeof(uint32_t), 1, compressed_file_reader);

    *num_points = (size_t)npts32;
    *num_pq_chunks = num_pq_chunks32;

    // 为压缩数据分配内存（用一维数组存储所有数据点的聚类中心索引）
    *compressed_data = (uint32_t *)malloc(*num_points * (*num_pq_chunks) * sizeof(uint32_t));

    num_blocks = (*num_points + 8192 - 1) / 8192; // 每个块最大 8192 个数据点
    block_size = (*num_points <= 8192) ? *num_points : 8192;

    // 逐块读取文件并直接存储到 compressed_data
    for (block = 0; block < num_blocks; block++)
    {
        size_t start_id = block * block_size;
        size_t end_id = (start_id + block_size > *num_points) ? *num_points : (start_id + block_size);
        size_t cur_blk_size = end_id - start_id;

        // 直接读取数据到 compressed_data
        size_t read_count = fread(*compressed_data + start_id * (*num_pq_chunks), sizeof(uint32_t), cur_blk_size * (*num_pq_chunks), compressed_file_reader);
        if (read_count != cur_blk_size * (*num_pq_chunks))
        {
            fprintf(stderr, "fread error: expected to read %zu elements, but read %zu elements\n", cur_blk_size * (*num_pq_chunks), read_count);
            fclose(compressed_file_reader);
            free(*compressed_data);
            return -1;
        }
    }

    fclose(compressed_file_reader);

    return 0;
}

// 示例函数：获取初始化 ID
void get_init_ids(uint32_t location, uint32_t *init_ids)
{
    // 模拟返回初始 ID（这部分需要根据实际情况修改）
    for (uint32_t i = 0; i < 10; i++)
    {
        init_ids[i] = (location + i) % 100;
    }
}

// 示例函数：获取向量数据
void get_vector(DataStore *data_store, uint32_t location, uint32_t *query)
{
    // 模拟获取数据
    query[0] = location;
}
float get_distance(float *vector1, float *vector2, size_t dim)
{
    float dist = 0.0;
    for (size_t i = 0; i < dim; i++)
    {
        dist += (vector1[i] - vector2[i]) * (vector1[i] - vector2[i]);
    }
    return dist;
}

void get_vec_from_compressed_data(const uint32_t *compressed_data, float *pivots_data, uint32_t location, float *vector, uint32_t num_pq_chunks, uint32_t dim)
{
    uint32_t *code = &compressed_data[location * num_pq_chunks];
    uint32_t subvector_dim = dim / num_pq_chunks;
    uint32_t num_centers = 256;

    // 解码每个chunk
    for (uint32_t chunk = 0; chunk < num_pq_chunks; ++chunk)
    {
        // 提取每个chunk对应的索引
        // 索引代表目标聚类中心向量的起始位置
        uint32_t index = *(code + chunk);

        // 找到查表的位置
        // float *pivot = pivots_data + (chunk * 256 + index) * subvector_dim;
        // float *pivot = pivots_data + index * dim + chunk * subvector_dim;
        float *pivot = pivots_data + (chunk * num_centers + index) * subvector_dim;

        // 拷贝这个子向量到vector中对应位置
        memcpy(vector + chunk * subvector_dim, pivot, sizeof(float) * subvector_dim);
    }
}

float vector_L2_distance(int dim, float *ax, float *bx)
{
    float distance = 0.0;

    /* Auto-vectorized */
    for (int i = 0; i < dim; i++)
    {
        float diff = ax[i] - bx[i];

        distance += diff * diff;
    }

    return distance;
}

float get_distance_by_id(uint32_t vec_a, uint32_t vec_b)
{
    // DEBUG
    // 从压缩向量中获取向量聚类中心
    // 再从码本中获取向量

    if (static_compressed_data == NULL || static_pivot_data == NULL)
    {
        elog(ERROR, "static_compressed_data or static_pivot_data is NULL");
        return 0.0;
    }

    float *vector_a = (float *)palloc(sizeof(float) * 128);
    get_vec_from_compressed_data(static_compressed_data, static_pivot_data, vec_a, vector_a, pq_chunk, 128);

    float *vector_b = (float *)palloc(sizeof(float) * 128);
    get_vec_from_compressed_data(static_compressed_data, static_pivot_data, vec_b, vector_b, pq_chunk, 128);
    float dist = get_distance(vector_a, vector_b, 128);
    pfree(vector_a);
    pfree(vector_b);
    return dist;
}

float get_distance_to_target_by_id(uint32_t vec_id, Vector *vec_b, uint32_t dim)
{
    // DEBUG
    // 从压缩向量中获取向量聚类中心
    // 再从码本中获取向量
    // elog(INFO, "get_distance starts");
    if (static_compressed_data == NULL || static_pivot_data == NULL)
    {
        elog(ERROR, "static_compressed_data or static_pivot_data is NULL");
        return 0.0;
    }

    float *vector_a = (float *)palloc(sizeof(float) * dim);
    get_vec_from_compressed_data(static_compressed_data, static_pivot_data, vec_id, vector_a, pq_chunk, 128);

    float *vector_b = vec_b->x;
    float dist = get_distance(vector_a, vector_b, dim);
    // elog(INFO, "get_distance ends");
    return dist;
}

// 初始化候选池并开始搜索
void iterate_to_fixed_point(Scratch *scratch, float *pivots_data, uint32_t *compressed_vectors, uint32_t *neighbours, uint32_t LSize, float *target_vector)
{
    // 1.初始化操作
    // 记录已访问的节点
    // 存储当前最优的L个邻居

    // 邻居集大小
    // 候选集
    NeighborPriorityQueue *L_nodes = scratch->best_L_nodes;
    // 起始点
    size_t init_id = scratch->entry_point;
    // 已访问节点
    size_t num_of_vec = scratch->max_point;
    uint32_t *is_visited = (uint32_t *)calloc(num_of_vec / 32 + 1, sizeof(uint32_t));
    // 已扩展的邻域
    NewVector *expanded_nodes = scratch->expanded_nodes;
    // 2.处理初始节点

    // 从 init_ids 中取出初始节点 ID，计算与查询向量的距离，并将其加入到 best_L_nodes 队列中。
    if (init_id > scratch->max_point)
    {
        elog(ERROR, "init_id is out of range");
    }
    set_bit(is_visited, (size_t)init_id); // 将初始节点标记为已访问

    // 将初始节点加入候选集
    float *init_vector = (float *)palloc(sizeof(float) * 128);
    get_vec_from_compressed_data(compressed_vectors, pivots_data, init_id, init_vector,pq_chunk, 128); // 计算距离
    float distance = get_distance(init_vector, target_vector, 128);
    pfree(init_vector); // 计算距离
    Neighbor nn;
    nn.id = init_id;
    nn.distance = distance;
    nn.expanded = false;
    priority_queue_insert(L_nodes, nn); // 将初始节点加入候选集

    // 用来存储搜索邻居的结果
    MyVector *id_scratch = (MyVector *)palloc(sizeof(MyVector));
    vector_init_with_capacity(id_scratch, 200);
    MyVector *dist_scratch = (MyVector *)palloc(sizeof(MyVector));
    vector_init_with_capacity(dist_scratch, 200);

    // 3.迭代过程 图搜索过程 获取候选集中未访问的节点
    while (has_unexpanded_node(L_nodes))
    {
        // 获取当前未被扩展的第一个节点
        Neighbor cur_node = closest_unexpanded(L_nodes);
        new_vector_push_back(expanded_nodes, &cur_node); // 将节点加入已扩展列表
        float distance;
        // 获取邻居
        // NewVector *cur_node_neighbours = new_vector_get(static_neighbors_vectors, cur_node.id);
        NewVector *cur_node_neighbours = new_vector_get(scratch->neighbors, cur_node.id);

        // 看看邻居是否在已访问列表中
        for (uint32_t i = 0; i < cur_node_neighbours->size; i++)
        {
            uint32_t *neighbor_id_pointer = new_vector_get(cur_node_neighbours, i);
            uint32_t neighbor_id = *neighbor_id_pointer;

            if (neighbor_id > scratch->max_point)
            {
                elog(ERROR, "neighbor_id is out of range");
                return;
            }
            if (test_bit(is_visited, (size_t)neighbor_id))
            {
                continue;
            }
            else
            {
                set_bit(is_visited, (size_t)neighbor_id); // 标记为已访问
            }

            // 计算该节点和距离
            float *cur_vector = (float *)palloc(sizeof(float) * 128);
            get_vec_from_compressed_data(compressed_vectors, pivots_data, neighbor_id, cur_vector, pq_chunk, 128); // 计算距离
            distance = get_distance(cur_vector, target_vector, 128);
            // 存入距离列表
            // neighbours_distances[i] = distance;
            vector_push_back(id_scratch, neighbor_id);
            vector_push_back(dist_scratch, distance);
            pfree(cur_vector);
        }

        // 将邻居加入候选池
        for (size_t i = 0; i < id_scratch->size; i++)
        {
            uint32_t neighbor_id = vector_get(id_scratch, i);
            float distance = vector_get(dist_scratch, i);
            Neighbor nn;
            nn.id = neighbor_id;
            nn.distance = distance;
            nn.expanded = false;
            priority_queue_insert(L_nodes, nn); // 将邻居加入候选集
            // elog(INFO, "neighbor_id: %u, distance: %f", neighbor_id, distance);
        }
    }
    // print distance
    // for (size_t i = 0; i < L_nodes->size; i++)
    // {
    //     Neighbor nn = L_nodes->data[i];
    //     elog(INFO, "id: %u, distance: %f", nn.id, nn.distance);
    // }
    vector_free(id_scratch);
    vector_free(dist_scratch);
    pfree(id_scratch);
    pfree(dist_scratch);
    free(is_visited);
}

void occlude_list(uint32_t location, NewVector *pool, float alpha, MyVector *pruned_list, uint32_t max_candidate_size, uint32_t R, Scratch *scratch)
{
    // 函数功能
    // 从一个距离排好序的候选邻居池中选择最多 degree 个邻居。
    // 使用 alpha 控制遮蔽机制，避免保留冗余或过于密集的邻居（提升图质量）。
    // 输出选中的节点id

    // 1.裁断候选集为maxc，即L
    if (pool->size == 0)
    {
        return;
    }
    if (pool->size > max_candidate_size)
    {
        new_vector_truncate(pool, (size_t)max_candidate_size);
    }
    // 2.每个邻居都有一个 occlude_factor（遮蔽因子），初始为0。后续会根据其他邻居的遮蔽影响逐渐升高。
    float *occlude_factor = (float *)palloc0((max_candidate_size + pool->size) * sizeof(float));

    // 3.迭代遮蔽过程
    // 进行多轮迭代，每轮的 cur_alpha 增大 1.2 倍。
    // 每轮试图选出一部分邻居，如果其 occlude_factor < cur_alpha，则可以保留。
    // 4.遍历候选邻居
    // 如果当前候选点被遮蔽程度太高（>cur_alpha），则跳过。
    // 否则加入 result，并将其遮蔽因子设为 float ::max，防止重复选入。
    // 然后更新它之后的邻居的 occlude_factor，表示它对后续点构成了“遮蔽”。

    // 3. 迭代遮蔽过程
    float cur_alpha = 1;
    while (cur_alpha <= alpha && pruned_list->size < R)
    {

        float eps = cur_alpha + 0.01f;
        for (size_t i = 0; pruned_list->size < R && i < pool->size; ++i)
        {
            if (occlude_factor[i] > cur_alpha)
            {
                continue;
            }

            // 更新 occlude_factor，设为最大值，防止重复选入
            occlude_factor[i] = FLT_MAX;

            // 检查是否被删除，且不与自己形成环路
            if (true)
            {
                Neighbor *cur = (Neighbor *)new_vector_get(pool, i);
                if (cur->id != location)
                {
                    // pruned_list[result_size++] = pool->data[i].id;
                    vector_push_back(pruned_list, cur->id);
                }
            }

            // 更新遮蔽因子
            for (size_t j = i + 1; j < pool->size; ++j)
            {
                if (occlude_factor[j] > alpha)
                {
                    continue;
                }

                bool prune_allowed = true;

                if (!prune_allowed)
                {
                    continue;
                }

                // 计算距离并更新遮蔽因子
                Neighbor *ni = (Neighbor *)new_vector_get(pool, i);
                Neighbor *nj = (Neighbor *)new_vector_get(pool, j);
                float djk = get_distance_by_id(ni->id, nj->id);
                // _dist_metric == diskann::Metric::L2
                occlude_factor[j] = (djk == 0) ? FLT_MAX : fmax(occlude_factor[j], nj->distance / djk);
            }
        }

        cur_alpha *= 1.2f;
    }

    // 如果需要的话，可以清理内存
    pfree(occlude_factor);
}

int compare_neighbor(const void *a, const void *b)
{
    const Neighbor *na = a;
    const Neighbor *nb = b;
    return (na->distance > nb->distance) - (na->distance < nb->distance);
}

// 示例函数：修剪邻居
void prune_neighbors(uint32_t location, Scratch *scratch, MyVector *pruned_list, const uint32_t max_candidate_size, float alpha, uint32_t R)
{
    // 空池直接返回
    if (scratch->expanded_nodes->size == 0)
    {
        return;
    }

    // 使用pg_dist，距离重新计算

    // 对expanded_nodes排序
    // 距离小的放前面
    new_vector_sort(scratch->expanded_nodes, compare_neighbor);

    // 清空pruned_list并预留空间R

    // 进行剪枝操作
    // uint32_t R = 10;
    NewVector *pool = scratch->expanded_nodes;
    occlude_list(location, pool, alpha, pruned_list, max_candidate_size, R, scratch);

    // 图饱和处理
    bool _saturate_graph = true;
    if (_saturate_graph && alpha > 1)
    {
        for (size_t i = 0; i < scratch->expanded_nodes->size; i++)
        {
            if (pruned_list->size >= R)
            {
                break;
            }
            Neighbor *nn = vector_get(scratch->expanded_nodes, i);
            if (vector_find(pruned_list, nn->id) == -1 && nn->id != location)
            {
                vector_push_back(pruned_list, nn->id);
            }
        }
    }
}

char *vector_to_string(NewVector *vec)
{
    if (!vec || vec->size == 0)
        return pstrdup("");

    size_t est_len = vec->size * 12 + 1;  // 每个元素最多12字节（含逗号空格），加末尾'\0'
    char *out = (char *)palloc0(est_len); // 初始化为0，避免拼接崩溃

    char temp[32];

    for (size_t i = 0; i < vec->size; i++)
    {
        uint32_t *val_ptr = new_vector_get(vec, i);
        uint32_t val = *val_ptr;

        snprintf(temp, sizeof(temp), "%u", val);
        strncat(out, temp, est_len - strlen(out) - 1);

        if (i < vec->size - 1)
        {
            strncat(out, ", ", est_len - strlen(out) - 1);
        }
    }

    return out; // 记得由调用者 pfree() 释放！
}

// 示例函数：修剪邻居
void new_prune_neighbors(uint32_t location, NewVector *pool, MyVector *pruned_list, const uint32_t max_candidate_size, float alpha, uint32_t R, Scratch *scratch)
{
    // 空池直接返回
    if (pool->size == 0)
    {
        vector_clear(pruned_list);
        return;
    }

    // 使用pg_dist，距离重新计算

    // 对expanded_nodes排序
    // 距离小的放前面
    // DEBUG
    new_vector_sort(pool, compare_neighbor);

    // 清空pruned_list并预留空间R
    vector_clear(pruned_list);
    vector_reserve(pruned_list, R);

    // 进行剪枝操作
    occlude_list(location, pool, alpha, pruned_list, max_candidate_size, R, scratch);
    assert(pruned_list->size <= R);
    // 图饱和处理
    bool _saturate_graph = true;
    if (_saturate_graph && alpha > 1)
    {
        for (size_t i = 0; i < pool->size; i++)
        {
            if (pruned_list->size >= R)
            {
                break;
            }
            Neighbor *nn = new_vector_get(pool, i);
            if (vector_find(pruned_list, nn->id) == -1 && nn->id != location)
            {
                vector_push_back(pruned_list, nn->id);
            }
        }
    }
}

// 搜索节点 加入候选集
void search_for_point_and_prune(Scratch *scratch, float *pivots_data, uint32_t *compressed_vectors, uint32_t *neighbours, uint32_t location, uint32_t Lindex, MyVector *pruned_list, float *query_vec, uint32_t R)
{

    // 执行固定点迭代 主要工作 从起始点开始，BFS，计算经过的点和距离加入pool
    iterate_to_fixed_point(scratch, pivots_data, compressed_vectors, neighbours, Lindex, query_vec);
    // exit(0);

    // 在最优候选集里面去除自己
    // DEBUG
    for (size_t i = 0; i < scratch->expanded_nodes->size; i++)
    {
        Neighbor *cur_neighbor = new_vector_get(scratch->expanded_nodes, i);
        if (cur_neighbor->id == location)
        {
            for (size_t j = i; j < scratch->expanded_nodes->size - 1; j++)
            {
                Neighbor *cur_neighbor = new_vector_get(scratch->expanded_nodes, j + 1);
                new_vector_set(scratch->expanded_nodes, j, cur_neighbor);
            }
            scratch->expanded_nodes->size--;
            i--;
        }
    }

    if (pruned_list->size > 0)
    {
        // elog(INFO, "pruned_list size %d", pruned_list->size);
        assert(pruned_list->size == 0);
    }

    // 调用修剪邻居
    float alpha = 1.20000005;
    // 最大遮蔽大小
    uint32_t _indexingMaxC = 100;
    // prune_neighbors(location, scratch, pruned_list, 50, alpha, R);
    NewVector *pool = scratch->expanded_nodes;
    new_prune_neighbors(location, pool, pruned_list, _indexingMaxC, alpha, R, scratch);
}

// 设置邻居
void set_neighbours(uint32_t node, MyVector *neighbors)
{
    // 这里实现将邻接列表存入 PostgreSQL 数据表
    if (neighbors->size == 0)
    {
        // elog(INFO, "neighbors size is 0");
        return;
    }
    NewVector *des_neighbors = NULL;
    des_neighbors = new_vector_get(static_neighbors_vectors, node);
    // elog(INFO, "Setting neighbors for node %u", node);
    new_vector_reserve(des_neighbors, neighbors->size);
    new_vector_resize(des_neighbors, neighbors->size);
    memcpy((char *)des_neighbors->data, (char *)neighbors->data, neighbors->size * sizeof(uint32_t));
    // check
    for (size_t i = 0; i < des_neighbors->size; i++)
    {
        uint32_t *cur_neighbor = new_vector_get(des_neighbors, i);
        // elog(INFO, "Neighbor %d", *cur_neighbor);
    }
}

void new_set_neighbours(uint32_t node, MyVector *neighbors, NewVector *neighbor_vectors)
{
    // 这里实现将邻接列表存入 PostgreSQL 数据表
    if (neighbors->size == 0)
    {
        // elog(INFO, "neighbors size is 0");
        return;
    }
    NewVector *des_neighbors = NULL;
    des_neighbors = new_vector_get(neighbor_vectors, node);
    // elog(INFO, "Setting neighbors for node %u", node);
    new_vector_reserve(des_neighbors, neighbors->size);
    new_vector_resize(des_neighbors, neighbors->size);
    memcpy((char *)des_neighbors->data, (char *)neighbors->data, neighbors->size * sizeof(uint32_t));
    // check
    for (size_t i = 0; i < des_neighbors->size; i++)
    {
        uint32_t *cur_neighbor = new_vector_get(des_neighbors, i);
        // elog(INFO, "Neighbor %d", *cur_neighbor);
    }
}

// 插入新边
void inter_insert(uint32_t node, MyVector *pruned_list, uint32_t R, Scratch *scratch)
{
    // `inter_insert` 函数的目的是将查询点 `n` 作为新的邻居插入到它的邻居节点的邻居池中（即 `pruned_list` 中的邻居）。
    // 如果目标节点的邻居池已满，
    // 函数会创建副本并进行剪枝，确保每个节点的邻居池不会过大，并且通过距离和其他准则优化邻居池。
    // 在操作过程中，为了确保线程安全，函数使用了锁来保护对邻居池的修改。
    // 最终，剪枝后的新邻居池会被写回到 `_graph_store` 中。
    elog(INFO, "Inter-inserting neighbors for node %u", node);
    assert(pruned_list->size != 0);
    uint32_t max_candidate_size = 100;

    for (size_t des = 0; des < pruned_list->size; des++)
    {
        bool prune_needed = false;
        uint32_t des_id = vector_get(pruned_list, des);
        assert(des_id < scratch->max_point);

        // 获取邻居
        // NewVector *des_neighbors = new_vector_get(static_neighbors_vectors, des_id);
        NewVector *des_neighbors = new_vector_get(scratch->neighbors, des_id);
        NewVector *copy_neighbors = NULL;

        // 查找邻域里面有没有node
        bool isfound = false;
        for (uint32_t i = 0; i < des_neighbors->size; i++)
        {
            uint32_t *cur_node_pointer = new_vector_get(des_neighbors, i);
            uint32_t cur_node = *cur_node_pointer;
            if (cur_node == node)
            {
                isfound = true;
                break;
            }
        }
        if (!isfound)
        {
            if (des_neighbors->size < (size_t)(GRAPH_SLACK_FACTOR * R))
            {
                new_vector_push_back(des_neighbors, &node);
                prune_needed = false;
            }
            else
            {
                // 将node加入到des的邻居 并剪枝
                // DEBUG
                copy_neighbors = (NewVector *)palloc(sizeof(NewVector));
                new_vector_init_with_capacity(copy_neighbors, sizeof(uint32_t), R + 10);
                for (size_t i = 0; i < des_neighbors->size; i++)
                {
                    uint32_t *cur_node_pointer = new_vector_get(des_neighbors, i);
                    uint32_t cur_node = *cur_node_pointer;
                    if (cur_node != node)
                    {
                        new_vector_push_back(copy_neighbors, cur_node_pointer);
                    }
                }
                new_vector_push_back(copy_neighbors, &node);
                prune_needed = true;
            }
        }

        if (prune_needed)
        {
            size_t reserveSize = (size_t)ceil(1.05 * GRAPH_SLACK_FACTOR * R);
            // 已访问列表
            size_t dummy_visited_size = (size_t)ceil(reserveSize / 32);
            uint32_t *dummy_visited = (uint32_t *)calloc(scratch->max_point / 32, sizeof(uint32_t));
            NewVector *dummy_pool = (NewVector *)palloc(sizeof(NewVector));
            new_vector_init_with_capacity(dummy_pool, sizeof(Neighbor), reserveSize);

            for (size_t i = 0; i < copy_neighbors->size; i++)
            {
                uint32_t *cur_node_pointer = new_vector_get(copy_neighbors, i);
                uint32_t cur_node = *cur_node_pointer;
                if (!test_bit(dummy_visited, (size_t)cur_node) && cur_node != des_id)
                {
                    float dist = get_distance_by_id(des_id, cur_node);
                    Neighbor cur_nbr = {cur_node, dist};
                    new_vector_push_back(dummy_pool, &cur_nbr);
                    set_bit(dummy_visited, (size_t)cur_node);
                }
            }
            MyVector *new_out_neighbors = (MyVector *)palloc(sizeof(MyVector));
            vector_init_with_capacity(new_out_neighbors, 50);
            // prune_neighbors(cur_node, scratch, new_out_neighbors, max_candidate_size, alpha, R);
            float alpha = 1.20000005;
            new_prune_neighbors(des_id, dummy_pool, new_out_neighbors, max_candidate_size, alpha, R, scratch);

            // set_neighbours
            // new_vector_reserve(des_neighbors, dummy_pool->size);
            // new_vector_resize(des_neighbors, dummy_pool->size);
            // memcpy((char *)des_neighbors->data, (char *)dummy_pool->data, dummy_pool->size * sizeof(uint32_t));
            new_set_neighbours(des_id, new_out_neighbors, scratch->neighbors);
            // set_neighbours(des_id, new_out_neighbors);

            new_vector_free(dummy_pool);
            pfree(dummy_pool);
            dummy_pool = NULL;
            free(dummy_visited);
            vector_free(new_out_neighbors);
            pfree(new_out_neighbors);
            new_out_neighbors = NULL;
        }
        if (copy_neighbors)
        {
            new_vector_free(copy_neighbors);
            pfree(copy_neighbors);
        }
    }
}

// 清除当前邻居
void clear_neighbours(uint32_t node)
{
    // 这里实现删除 PostgreSQL 中的邻接数据
    elog(INFO, "Clearing neighbors for node %u", node);
}

// Vamana 链接过程
void vamana_link(float *pivots_data, uint32_t *compressed_vectors, uint32_t *neighbours, uint32_t dim, size_t num_points, uint32_t R, uint32_t L, uint32_t num_threads)
{

    // 初始化邻接表
    NewVector *my_neighbors_vectors = (NewVector *)malloc(sizeof(NewVector));
    generate_random_neighbors_for_vector_empty(my_neighbors_vectors, (size_t)num_points, (size_t)R);

    // BFS贪心算法
    // 执行搜索，生成候选集
    // 从候选池中移除当前节点自身，避免自连接。

    // 剪枝
    // 1.若候选池为空，直接返回空列表
    // 2.PQ距离替换
    // 3.调用遮挡剪枝
    // 4. 饱和度补充

    //

    size_t i;
    // 要存储初始点
    // size_t entry_point = calculate_entry(num_points);
    size_t entry_point = 3732;

    // 遍历列表
    for (i = 0; i < num_points; i++)
    {

        Scratch *scratch = (Scratch *)palloc(sizeof(Scratch));
        init_scratch(scratch, num_points, L, entry_point, i);
        scratch->neighbors = my_neighbors_vectors;
        MyVector *pruned_list = (MyVector *)palloc(sizeof(MyVector));
        vector_init(pruned_list);
        float *query = (float *)palloc0(sizeof(float) * dim);
        get_vec_from_compressed_data(compressed_vectors, pivots_data, i, query, pq_chunk, dim);

        search_for_point_and_prune(scratch, pivots_data, compressed_vectors, neighbours, i, L, pruned_list, query, R);
        assert(pruned_list->size > 0);
        // #pragma omp critical
        if (pruned_list->size == 0)
        {
            // 打印scratch
            elog(INFO, "entry point:%d", entry_point);
            elog(ERROR, "pruned_list is empty,node:%d", i);
            elog(INFO, "pool size:%d", scratch->expanded_nodes->size);
            elog(INFO, "L_NODE size:%d", scratch->best_L_nodes->size);
        }

        // set_neighbours(i, pruned_list);
        new_set_neighbours(i, pruned_list, my_neighbors_vectors);
        // 打印
        // NewVector *cur_neighbors = new_vector_get(static_neighbors_vectors, i);
        // NewVector *cur_neighbors = new_vector_get(my_neighbors_vectors, i);
        // char *msg = vector_to_string(cur_neighbors);
        // elog(INFO, "node:%d,neighbours:%d,neighbor:%s", i, cur_neighbors->size, msg);
        // pfree(msg);

        inter_insert(i, pruned_list, R, scratch);

        // elog(INFO, "pruned_list");
        vector_free(pruned_list);
        // elog(INFO, "pruned_list end");
        pfree(pruned_list);
        if (query)
        {
            pfree(query);
            query = NULL;
        }

        free_scratch(scratch);
        pfree(scratch);
    }

    // for (size_t i = 0; i < static_neighbors_vectors->size; i++)
    // {
    //     NewVector *cur_neighbors = new_vector_get(static_neighbors_vectors, i);
    //     char *msg = vector_to_string(cur_neighbors);
    //     elog(INFO, "node:%d,neighbours:%d,neighbor:%s", i, cur_neighbors->size, msg);
    //     pfree(msg);
    // }

    // 最终剪枝
    elog(INFO, "Final pruning started.");
    // #pragma omp parallel for schedule(dynamic, 2048)
    for (i = 0; i < num_points; i++)
    {
        // 如果一个节点的度数大于R,那么剪枝
        NewVector *cur_neighbors = NULL;
        // cur_neighbors = new_vector_get(static_neighbors_vectors, i);
        cur_neighbors = new_vector_get(my_neighbors_vectors, i);
        // Scratch *scratch = (Scratch *)palloc(sizeof(Scratch));
        // init_scratch(scratch, num_points, L, entry_point, i);
        if (cur_neighbors->size > R)
        {
            // 已访问列表
            uint32_t *dummy_visited = (uint32_t *)calloc(num_points / 32 + 1, sizeof(uint32_t));
            NewVector *dummy_pool = (NewVector *)palloc(sizeof(NewVector));
            new_vector_init_with_capacity(dummy_pool, sizeof(Neighbor), 2 * R);
            MyVector *new_out_neighbors = (MyVector *)palloc(sizeof(MyVector));
            vector_init(new_out_neighbors);
            for (size_t j = 0; j < cur_neighbors->size; j++)
            {
                uint32_t *cur_node_pointer = new_vector_get(cur_neighbors, j);
                uint32_t cur_node = *cur_node_pointer;
                // 如果节点不在已访问列表中 且不是自身
                if (!test_bit(dummy_visited, (size_t)cur_node) && cur_node != i)
                {
                    float dist = get_distance_by_id(i, cur_node);
                    Neighbor nn;
                    nn.id = cur_node;
                    nn.distance = dist;
                    nn.expanded = false;
                    new_vector_push_back(dummy_pool, &nn);
                    set_bit(dummy_visited, (size_t)cur_node);
                }
            }
            float alpha = 1.20000005;
            // prune_neighbors(i, scratch, dummy_pool, L, 1.0, R);
            new_prune_neighbors(i, dummy_pool, new_out_neighbors, L, alpha, R, NULL);
            // set_neighbours(i, new_out_neighbors);
            new_set_neighbours(i, new_out_neighbors, my_neighbors_vectors);
            if (dummy_pool)
                pfree(dummy_pool);
            if (dummy_visited)
                free(dummy_visited);
            if (new_out_neighbors)
                vector_free(new_out_neighbors);
            pfree(new_out_neighbors);
        }
        // free_scratch(scratch);
        // pfree(scratch);
        // prune_neighbors();
    }
    // 存储邻居
    new_save_neighbors_to_disk(my_neighbors_vectors, "vectors_index_table");

    // for (size_t i = 0; i < my_neighbors_vectors->size; i++)
    // {
    //     NewVector *cur_neighbors = new_vector_get(my_neighbors_vectors, i);
    //     char *msg = vector_to_string(cur_neighbors);
    //     elog(INFO, "node:%d,neighbours:%d,neighbor:%s", i, cur_neighbors->size, msg);
    //     pfree(msg);
    // }
    elog(INFO, "Linking completed.");
}

static double estimate_ram_usage(size_t num_points, uint32_t dim, size_t data_size, uint32_t R)
{
    return (double)(num_points * dim * data_size * R) / (1024.0 * 1024.0 * 1024.0);
}

// 创建 Vamana 索引
NewVector *build(const char *compressed_vec_file, const char *pivots_file, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t num_pq_chunks)
{
    uint32_t num_threads_index = num_threads;
    uint32_t index_L = L;
    uint32_t maxc = 50; // 候选集
    float *full_pivot_data = NULL;
    uint32_t num_centers = 0;
    uint32_t dim = 0;
    float *centroid = NULL;
    size_t *chunk_offsets = NULL;
    // 入口点 是搜索算法的起点
    uint32_t entry_point = 0;
    uint32_t num_points = 0; // TODO 这里可能要改成size_t
    uint32_t *compressed_data;
    size_t ret;
    // 存储
    // ​存储经过剪枝（Pruning）后的邻居节点列表

    // 加载码本
    /* Load pivot data */
    ret = load_pq_pivots(pivots_file, &full_pivot_data, &num_centers, &dim, &centroid, &chunk_offsets, &num_pq_chunks);

    // 加载压缩向量
    // 1.向量数
    // 2.分块数
    if (load_compressed_vectors(compressed_vec_file, &num_points, &num_pq_chunks, &compressed_data) != 0)
    {
        elog(ERROR, "Error loading compressed vectors");
        return;
    }
    static_compressed_data = compressed_data;
    static_pivot_data = full_pivot_data;

    // 生成随机邻居
    uint32_t *neighbors = NULL;
    NewVector *neighbors_vectors = (NewVector *)malloc(sizeof(NewVector));
    // generate_random_neighbors_for_vector(neighbors_vectors, num_points, R);
    generate_random_neighbors_for_vector_empty(neighbors_vectors, (size_t)num_points, (size_t)R);
    static_neighbors_vectors = neighbors_vectors;
    // check neighbors
    // elog(INFO, "check neighbors size");
    // for (size_t i = 0; i < num_points; i++)
    // {
    //     NewVector *cur_neighbors = new_vector_get(neighbors_vectors, i);
    //     for (size_t j = 0; j < cur_neighbors->size; j++)
    //     {
    //         elog(INFO, "neighbors size,%d", cur_neighbors->size);
    //     }
    // }
    vamana_link(full_pivot_data, compressed_data, neighbors, dim, num_points, R, L, num_threads);
    // pfree(neighbors);
    return neighbors_vectors;
}

NewVector *build_merged_vamana_index(const char *pivots_data, const char *compressed_vec, double ram_budget, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t base_num, uint32_t base_dim)
{
    double full_index_ram = estimate_ram_usage(base_num, (uint32_t)base_dim, sizeof(float), R);

    if (full_index_ram < ram_budget * 1024 * 1024 * 1024)
    {
        elog(INFO, "Full index fits in RAM budget: %.2f GiB", full_index_ram / (1024 * 1024 * 1024));

        // 链接
        NewVector *neighbor_vec = build(compressed_vec, pivots_data, R, L, num_threads, 8);
        // 邻居度数统计

        // 删除不必要的文件
        // unlink(medoids_file);
        // unlink(centroids_file);
        return neighbor_vec;
    }

    elog(INFO, "Index exceeds RAM budget, using partitioning");

    // 省略分片和合并索引的实现

    return NULL;
}