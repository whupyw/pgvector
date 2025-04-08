#include "postgres.h"
#include "utils/memutils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include "new_vector.h"
#include <stdbool.h>
#include "vector.h"

void swap(int *a, int *b);

float get_distance_by_id(uint32_t vec_a, uint32_t vec_b);

float get_distance_to_target_by_id(uint32_t vec_id, Vector *vec_b, uint32_t dim);

void get_neighbors(uint32_t point_index, uint32_t R, uint32_t *neighbors, uint32_t *result_neighbors);

void generate_random_neighbors(uint32_t num_points, uint32_t R, uint32_t *neighbors);

bool generate_random_neighbors_for_vector(NewVector *vec, uint32_t num_points, uint32_t R);

int build_merged_vamana_index(const char *pivots_data, const char *compressed_vec, double ram_budget, uint32_t R, uint32_t L, uint32_t num_threads, uint32_t base_num, uint32_t base_dim);

float vector_L2_distance(int dim, float *ax, float *bx);