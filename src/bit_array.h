#ifndef BITARRAY_H
#define BITARRAY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void set_bit(uint32_t *bit_array, size_t index);
void clear_bit(uint32_t *bit_array, size_t index);
bool test_bit(uint32_t *bit_array, size_t index);

#endif
