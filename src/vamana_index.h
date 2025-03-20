#include "postgres.h"
#include "utils/memutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

int build_merged_vamana_index(const char *base_file, uint32_t L, uint32_t R,
                              double sampling_rate, double ram_budget, const char *mem_index_path,
                              const char *medoids_file, const char *centroids_file, size_t build_pq_bytes,
                              uint32_t num_threads, uint32_t Lf, size_t base_num, size_t base_dim);