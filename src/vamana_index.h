#include "postgres.h"
#include "utils/memutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

void swap(int *a, int *b);

void get_neighbors(uint32_t point_index, uint32_t R, uint32_t *neighbors, uint32_t *result_neighbors);

void generate_random_neighbors(uint32_t num_points, uint32_t R, uint32_t *neighbors);

int build_merged_vamana_index(const char *pivots_data, const char *compressed_vec, double ram_budget, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t base_num, uint32_t base_dim);