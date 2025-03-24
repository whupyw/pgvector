#ifndef BITARRAY_H
#define BITARRAY_H
#include <stdint.h>
#include <stddef.h>

void set_bit(uint32_t *bit_array, size_t index)
{
    bit_array[index / 32] |= (1U << (index % 32)); // 设置特定位为1
}

void clear_bit(uint32_t *bit_array, size_t index)
{
    bit_array[index / 32] &= ~(1U << (index % 32)); // 设置特定位为0
}

int test_bit(uint32_t *bit_array, size_t index)
{
    return (bit_array[index / 32] >> (index % 32)) & 1; // 获取特定位的值
}

#endif
