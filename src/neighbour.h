#ifndef NEIGHBOUR_H
#define NEIGHBOUR_H

#include "postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t id;
    float distance;
    bool expanded;
} Neighbor;

typedef struct
{
    Neighbor *data;
    size_t size;
    size_t capacity;
    size_t cur; // Index of the next unexpanded node
} NeighborPriorityQueue;

void init_queue(NeighborPriorityQueue *queue, size_t capacity);
int compare_neighbors(const void *a, const void *b);
void priority_queue_insert(NeighborPriorityQueue *queue, Neighbor nbr);
Neighbor closest_unexpanded(NeighborPriorityQueue *queue);
bool has_unexpanded_node(NeighborPriorityQueue *queue);
void priority_queue_reserve(NeighborPriorityQueue *queue, size_t capacity);
void clear_queue(NeighborPriorityQueue *queue);
void free_queue(NeighborPriorityQueue *queue);

#endif
