#include "vamana_index.h"
#include "diskann.h"
#include "my_vector.h"
#include "scratch.h"
#include <stdlib.h>

typedef struct
{
    uint32_t *location_to_labels;
    uint32_t *label_to_start_id;
    uint32_t *aligned_query;
    uint32_t max_points;
    uint32_t num_frozen_pts;
    uint32_t *graph_store;
} DataStore;

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
    *compressed_data = (uint32_t *)palloc(*num_points * (*num_pq_chunks) * sizeof(uint32_t));

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

void get_vec_from_compressed_data(const float *compressed_data, float *pivots_data, uint32_t location, float *vector, uint32_t num_pq_chunks, uint32_t dim)
{
    uint32_t *code = &compressed_data[location];
    uint32_t subvector_dim = dim / num_pq_chunks;

    // 解码每个chunk
    for (uint32_t chunk = 0; chunk < num_pq_chunks; ++chunk)
    {
        // 提取每个chunk对应的索引
        // 索引代表目标聚类中心向量的起始位置
        uint32_t index = *(code + chunk);

        // 找到查表的位置
        // float *pivot = pivots_data + (chunk * 256 + index) * subvector_dim;
        float *pivot = pivots_data + index * dim + chunk * subvector_dim;

        // 拷贝这个子向量到vector中对应位置
        memcpy(vector + chunk * subvector_dim, pivot, sizeof(float) * subvector_dim);
    }
}

void get_vec_from_vectors(float *vectors, uint32_t location, float *vector, uint32_t dim)
{
    // 从原始数据中获取向量
    // 计算这两个向量的距离
    // 返回距离
}

// 初始化候选池并开始搜索
/*
 * Performs iterative search to find nearest neighbors using Vamana graph index.
 *
 * This function implements the core graph traversal algorithm of Vamana index.
 * Starting from an initial node, it iteratively explores neighbors to find the
 * closest points to the target vector, using a priority queue to maintain the
 * best L candidates found so far.
 *
 * @param scratch          Workspace for temporary data structures
 * @param pivots_data     Pivot vectors for compressed data reconstruction
 * @param compressed_vectors Compressed vector data
 * @param neighbours      Graph adjacency lists
 * @param LSize          Size of candidate pool to maintain
 * @param init_id        Initial node ID to start search from
 * @param target_vector  Query vector to find neighbors for
 */
void iterate_to_fixed_point(Scratch *scratch, float *pivots_data, uint32_t *compressed_vectors, uint32_t *neighbours, uint32_t LSize, float *target_vector)
{
    // 1.初始化操作
    // 记录已访问的节点
    // 存储当前最优的L个邻居
    // 使用bitset
    uint32_t R = 10;
    // 候选集
    NeighborPriorityQueue *L_nodes = scratch->best_L_nodes;
    // init_queue(&L_nodes, LSize);
    //  读取向量个数
    uint32_t max_points = 25000; // 最大节点数
    size_t num_bits = 100;       // 需要存储 100 位
    size_t num_ints = (num_bits + 31) / 32;
    size_t init_id = scratch->entry_point;      // 计算需要多少 uint32_t
    uint32_t *is_visited = scratch->is_visited; // 创建一个 bit 数组并初始化为0
                                                // 已扩展的节点列表
    MyVector *expanded_nodes = scratch->expanded_nodes;
    // 申请内存用于存储已访问邻居
    // id_和 dist_临时存储 ID 和距离数据，用于计算节点之间的距离。
    // uint32_t *id_scratch = (uint32_t *)palloc(sizeof(uint32_t) * 1024); // 估计大小
    // float *dist_scratch = (float *)palloc(sizeof(float) * 1024);        // 估计大小

    // 2.处理初始节点

    // 从 init_ids 中取出初始节点 ID，计算与查询向量的距离，并将其加入到 best_L_nodes 队列中。
    if (init_id > scratch->max_point)
    {
        elog(ERROR, "init_id is out of range");
    }
    set_bit(is_visited, init_id); // 将初始节点标记为已访问

    // 将初始节点加入候选集
    float *init_vector = (float *)palloc(sizeof(float) * 128);
    get_vec_from_compressed_data(compressed_vectors, pivots_data, init_id, init_vector, 8, 128); // 计算距离
    float distance = get_distance(init_vector, target_vector, 128);                              // 计算距离
    Neighbor nn;
    nn.id = init_id;
    nn.distance = distance;
    priority_queue_insert(L_nodes, nn); // 将初始节点加入候选集

    // 用来存储搜索邻居的结果
    // uint32_t *cur_node_neighbors = (uint32_t *)palloc(sizeof(uint32_t) * R); // 估计大小
    // float *neighbours_distances = (float*)palloc(sizeof(float)*R); // 估计大小

    MyVector *id_scratch = (MyVector *)palloc(sizeof(MyVector));
    vector_init(id_scratch);
    MyVector *dist_scratch = (MyVector *)palloc(sizeof(MyVector));
    vector_init(dist_scratch);

    // 3.迭代过程 图搜索过程
    while (has_unexpanded_node(L_nodes))
    {
        // 获取当前未被扩展的第一个节点
        Neighbor cur_node = closest_unexpanded(L_nodes);
        vector_push_back(expanded_nodes, cur_node.id); // 将节点加入已扩展列表
        float distance;

        // 看看邻居是否在已访问列表中
        for (uint32_t i = 0; i < R; i++)
        {
            uint32_t neighbor_id = neighbours[cur_node.id * R + i];
            if (neighbor_id > scratch->max_point)
            {
                elog(ERROR, "neighbor_id is out of range");
                return;
            }
            if (test_bit(is_visited, neighbor_id))
            {
                continue;
            }
            set_bit(is_visited, neighbor_id); // 标记为已访问

            // 计算距离
            float *cur_vector = (float *)palloc(sizeof(float) * 128);
            get_vec_from_compressed_data(compressed_vectors, pivots_data, neighbor_id, cur_vector, 8, 128); // 计算距离
            distance = get_distance(cur_vector, target_vector, 128);
            // 存入距离列表
            // neighbours_distances[i] = distance;
            vector_push_back(id_scratch, neighbor_id);
            vector_push_back(dist_scratch, distance);
        }

        // 将邻居加入候选池
        for (size_t i = 0; i < id_scratch->size; i++)
        {
            uint32_t neighbor_id = vector_get(id_scratch, i);
            float distance = vector_get(dist_scratch, i);
            Neighbor nn;
            nn.id = neighbor_id;
            nn.distance = distance;
            priority_queue_insert(L_nodes, nn); // 将邻居加入候选集
            //elog(INFO, "neighbor_id: %u, distance: %f", neighbor_id, distance);
        }
    }
    for (size_t i = 0; i < L_nodes->size; i++)
    {
        Neighbor nn = L_nodes->data[i];
        elog(INFO, "id: %u, distance: %f", nn.id, nn.distance);
    }
}

// 示例函数：修剪邻居
void prune_neighbors(uint32_t location, Neighbor *pool, size_t pool_size, uint32_t *pruned_list)
{
    // 模拟修剪邻居，选择最近的邻居
    for (size_t i = 0; i < pool_size; i++)
    {
        pruned_list[i] = pool[i].id;
    }
}

// 搜索节点 加入候选集
void search_for_point_and_prune(Scratch *scratch, float *pivots_data, uint32_t *compressed_vectors, uint32_t *neighbours, uint32_t location, uint32_t Lindex, uint32_t *pruned_list, float *query_vec)
{

    // 执行固定点迭代 主要工作 从起始点开始，BFS，计算经过的点和距离加入pool
    iterate_to_fixed_point(scratch, pivots_data, compressed_vectors, neighbours, Lindex, query_vec);
    //exit(0);

    // 在最优候选集里面去除自己
    for(size_t i = 0; i < scratch->expanded_nodes->size; i++)
    {
        if (scratch->expanded_nodes->data[i] == location)
        {
            for (size_t j = i; j < scratch->expanded_nodes->size - 1; j++)
            {
                scratch->expanded_nodes->data[j] = scratch->expanded_nodes->data[j + 1];
            }
            scratch->expanded_nodes->size--;
            i--;
        }
    }

    // for (size_t i = 0; i < scratch->pool_size; i++)
    // {
    //     if (scratch->pool[i].id == location)
    //     {
    //         for (size_t j = i; j < scratch->pool_size - 1; j++)
    //         {
    //             scratch->pool[j] = scratch->pool[j + 1];
    //         }
    //         scratch->pool_size--;
    //         i--;
    //     }
    // }

    // 调用修剪邻居
    // prune_neighbors(location, scratch->pool, scratch->pool_size, pruned_list);
}

// 设置邻居
void set_neighbours(uint32_t node, uint32_t *neighbors, uint32_t R)
{
    // 这里实现将邻接列表存入 PostgreSQL 数据表
    elog(INFO, "Setting neighbors for node %u", node);
}

// 插入新边
void inter_insert(uint32_t node, uint32_t *pruned_list)
{
    // 这里实现互相插入邻居
    elog(INFO, "Inter-inserting neighbors for node %u", node);
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
    size_t entry_point = calculate_entry(num_points);

    // 遍历列表
    for (i = 0; i < num_points; i++)
    {
        Scratch *scratch = (Scratch *)palloc(sizeof(Scratch));
        init_scratch(scratch, num_points, L, entry_point);
        uint32_t *pruned_list = (uint32_t *)palloc(R * sizeof(uint32_t));
        if (!pruned_list)
        {
            elog(ERROR, "Memory allocation failed");
        }
        float *query = (float *)palloc(sizeof(float) * dim);
        get_vec_from_compressed_data(compressed_vectors, pivots_data, i, query, 8, dim);

        search_for_point_and_prune(scratch, pivots_data, compressed_vectors, neighbours, i, L, pruned_list, query);

#pragma omp critical
        {
            set_neighbours(i, pruned_list, R);
        }

        inter_insert(i, pruned_list);

        pfree(pruned_list);
        pfree(query);
        pfree(scratch);
    }

// 最终剪枝
#pragma omp parallel for schedule(dynamic, 2048)
    for (i = 0; i < num_points; i++)
    {
        // 如果一个节点的度数大于R,那么剪枝
        uint32_t *new_out_neighbors = (uint32_t *)palloc(R * sizeof(uint32_t));
        if (!new_out_neighbors)
        {
            elog(ERROR, "Memory allocation failed");
        }

        // prune_neighbors();

#pragma omp critical
        {
            clear_neighbours(i);
            set_neighbours(i, new_out_neighbors, R);
        }

        pfree(new_out_neighbors);
    }

    elog(INFO, "Linking completed.");
}

static double estimate_ram_usage(size_t num_points, uint32_t dim, size_t data_size, uint32_t R)
{
    return (double)(num_points * dim * data_size * R) / (1024.0 * 1024.0 * 1024.0);
}

// 创建 Vamana 索引
void build(const char *compressed_vec_file, const char *pivots_file, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t num_pq_chunks)
{
    uint32_t index_R = R;
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

    // 生成随机邻居 
    uint32_t *neighbors = (uint32_t *)palloc(num_points * R * sizeof(uint32_t));
    generate_random_neighbors(num_points, R, neighbors);

    vamana_link(full_pivot_data, compressed_data, neighbors, dim, num_points, R, L, num_threads);
    pfree(neighbors);
}

int build_merged_vamana_index(const char *pivots_data, const char *compressed_vec, double ram_budget, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t base_num, uint32_t base_dim)
{
    double full_index_ram = estimate_ram_usage(base_num, (uint32_t)base_dim, sizeof(float), R);

    if (full_index_ram < ram_budget * 1024 * 1024 * 1024)
    {
        elog(INFO, "Full index fits in RAM budget: %.2f GiB", full_index_ram / (1024 * 1024 * 1024));

        // 链接
        build(compressed_vec, pivots_data, R, L, num_threads, 8);
        // 邻居度数统计

        // 删除不必要的文件
        // unlink(medoids_file);
        // unlink(centroids_file);
        return 0;
    }

    elog(INFO, "Index exceeds RAM budget, using partitioning");

    // 省略分片和合并索引的实现

    return 0;
}