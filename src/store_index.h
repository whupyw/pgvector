#ifndef STORE_INDEX_H
#define STORE_INDEX_H
#include <stdbool.h>
#include <stdint.h>
#include "new_vector.h"
bool create_disk_laylout(const char *table_name, NewVector *static_neighbors_vectors);

bool save_neighbors_to_disk(NewVector *neighbors, const char *table_name);

#endif