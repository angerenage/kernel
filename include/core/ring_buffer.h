#pragma once

#include <core/lock.h>
#include <core/spinlock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A general-purpose fixed-capacity ring buffer that stores elements of a
 * caller-specified size. The buffer allocates its storage and
 * provides lock-protected enqueue/dequeue operations.
 */
struct ring_buffer {
	struct spinlock lock;
	size_t          head;
	size_t          tail;
	size_t          count;
	size_t          capacity;
	size_t          element_size;
	uint8_t*        data;
};

/* Initialize a ring buffer with the given capacity and element size. */
bool ring_buffer_init(struct ring_buffer* rb, const char* lock_name, uint32_t lock_order, size_t capacity,
                      size_t element_size);

/* Release the ring buffer's storage. */
void ring_buffer_deinit(struct ring_buffer* rb);

/* Enqueue a copy of element. Returns true on success, false when full. */
bool ring_buffer_enqueue(struct ring_buffer* rb, const void* element);

/* Dequeue the oldest element into out_element. Returns true on success, false when empty. */
bool ring_buffer_dequeue(struct ring_buffer* rb, void* out_element);

/* Peek at the oldest element without removing it. Returns true on success, false when empty. */
bool ring_buffer_peek(struct ring_buffer* rb, void* out_element);

/* Return the number of elements currently in the buffer. */
size_t ring_buffer_count(struct ring_buffer* rb);
