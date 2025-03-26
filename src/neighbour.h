#ifndef NEIGHBOUR_H
#define NEIGHBOUR_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

// Function to compare two neighbors based on their distance and id
int compare_neighbors(const void *a, const void *b)
{
    Neighbor *neighbor_a = (Neighbor *)a;
    Neighbor *neighbor_b = (Neighbor *)b;
    if (neighbor_a->distance < neighbor_b->distance)
        return -1;
    if (neighbor_a->distance > neighbor_b->distance)
        return 1;
    return neighbor_a->id < neighbor_b->id ? -1 : (neighbor_a->id > neighbor_b->id ? 1 : 0);
}

// Initialize the priority queue
void init_queue(NeighborPriorityQueue *queue, size_t capacity)
{
    queue->data = (Neighbor *)malloc((capacity + 1) * sizeof(Neighbor)); // +1 for 1-based indexing
    queue->size = 0;
    queue->capacity = capacity;
    queue->cur = 0;
}

// Insert a neighbor into the queue
void priority_queue_insert(NeighborPriorityQueue *queue, Neighbor nbr)
{
    if (queue->size == queue->capacity && queue->data[queue->size - 1].distance < nbr.distance)
    {
        return; // If the queue is full and the new neighbor's distance is larger than the farthest, do nothing
    }

    size_t lo = 0, hi = queue->size;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (compare_neighbors(&nbr, &queue->data[mid]) < 0)
        {
            hi = mid;
        }
        else if (queue->data[mid].id == nbr.id)
        {
            return; // If the id already exists, do nothing
        }
        else
        {
            lo = mid + 1;
        }
    }

    // Insert the neighbor at position 'lo'
    if (lo < queue->capacity)
    {
        memmove(&queue->data[lo + 1], &queue->data[lo], (queue->size - lo) * sizeof(Neighbor));
    }
    queue->data[lo] = nbr;
    if (queue->size < queue->capacity)
    {
        queue->size++;
    }
    if (lo < queue->cur)
    {
        queue->cur = lo;
    }
}

// Get the closest unexpanded neighbor
Neighbor closest_unexpanded(NeighborPriorityQueue *queue)
{
    queue->data[queue->cur].expanded = true;
    size_t pre = queue->cur;
    while (queue->cur < queue->size && queue->data[queue->cur].expanded)
    {
        queue->cur++;
    }
    return queue->data[pre];
}

// Check if there are any unexpanded nodes in the queue
bool has_unexpanded_node(NeighborPriorityQueue *queue)
{
    return queue->cur < queue->size;
}

// Reserve space for the queue
void priority_queue_reserve(NeighborPriorityQueue *queue, size_t capacity)
{
    if (capacity + 1 > queue->capacity)
    {
        queue->data = (Neighbor *)realloc(queue->data, (capacity + 1) * sizeof(Neighbor));
        queue->capacity = capacity;
    }
}

// Clear the queue
void clear_queue(NeighborPriorityQueue *queue)
{
    queue->size = 0;
    queue->cur = 0;
}

// Free the allocated memory
void free_queue(NeighborPriorityQueue *queue)
{
    free(queue->data);
}

#endif
