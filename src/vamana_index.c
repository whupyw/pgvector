#include "vamana_index.h"



static double estimate_ram_usage(size_t num_points, uint32_t dim, size_t data_size, uint32_t R)
{
    return (double)(num_points * dim * data_size * R) / (1024.0 * 1024.0 * 1024.0);
}

void vamana_link(){
    elog(INFO, "Linking Vamana index");
}

// 创建 Vamana 索引
void build(uint16_t dim, size_t num_points, const char *table_name, uint32_t R, uint32_t L, uint32_t num_threads)
{
    uint32_t index_R = R;
    uint32_t num_threads_index = num_threads;
    uint32_t index_L = L;
    uint32_t maxc = 50;//候选集
    vamana_link();
}

int build_merged_vamana_index(const char *base_file, uint32_t L, uint32_t R,
                              double sampling_rate, double ram_budget, const char *mem_index_path,
                              const char *medoids_file, const char *centroids_file, size_t build_pq_bytes,
                              uint32_t num_threads, uint32_t Lf, size_t base_num, size_t base_dim)
{
    double full_index_ram = estimate_ram_usage(base_num, (uint32_t)base_dim, sizeof(float), R);

    if (full_index_ram < ram_budget * 1024 * 1024 * 1024)
    {
        elog(INFO, "Full index fits in RAM budget: %.2f GiB", full_index_ram / (1024 * 1024 * 1024));

        // 链接

        // 邻居度数统计

        // 保存索引
        elog(INFO, "Saving index to: %s", mem_index_path);

        // 删除不必要的文件
        //unlink(medoids_file);
        //unlink(centroids_file);
        return 0;
    }

    elog(INFO, "Index exceeds RAM budget, using partitioning");

    // 省略分片和合并索引的实现

    return 0;
}