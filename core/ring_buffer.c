#include <core/ring_buffer.h>
#include <core/spinlock.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool ring_buffer_init(struct ring_buffer* rb, const char* lock_name, uint32_t lock_order, size_t capacity,
                      size_t element_size) {
	size_t allocation_size;

	if (rb == NULL || capacity == 0u || element_size == 0u) return false;
	if (capacity > SIZE_MAX / element_size) return false;
	allocation_size = capacity * element_size;

	rb->data = malloc(allocation_size);
	if (rb->data == NULL) return false;

	spinlock_init_class(&rb->lock, lock_name, lock_order, SPINLOCK_FLAG_IRQSAVE);
	rb->head         = 0u;
	rb->tail         = 0u;
	rb->count        = 0u;
	rb->capacity     = capacity;
	rb->element_size = element_size;
	memset(rb->data, 0, allocation_size);
	return true;
}

void ring_buffer_deinit(struct ring_buffer* rb) {
	if (rb == NULL) return;
	free(rb->data);
	rb->data         = NULL;
	rb->head         = 0u;
	rb->tail         = 0u;
	rb->count        = 0u;
	rb->capacity     = 0u;
	rb->element_size = 0u;
}

bool ring_buffer_enqueue(struct ring_buffer* rb, const void* element) {
	struct irq_state state;
	uint8_t*         dst;

	if (rb == NULL || element == NULL) return false;

	state = spinlock_lock_irqsave(&rb->lock);
	if (rb->count >= rb->capacity) {
		spinlock_unlock_irqrestore(&rb->lock, state);
		return false;
	}

	dst = rb->data + rb->tail * rb->element_size;
	memcpy(dst, element, rb->element_size);
	rb->tail = (rb->tail + 1u) % rb->capacity;
	rb->count++;
	spinlock_unlock_irqrestore(&rb->lock, state);
	return true;
}

bool ring_buffer_dequeue(struct ring_buffer* rb, void* out_element) {
	struct irq_state state;
	const uint8_t*   src;

	if (rb == NULL || out_element == NULL) return false;

	state = spinlock_lock_irqsave(&rb->lock);
	if (rb->count == 0u) {
		spinlock_unlock_irqrestore(&rb->lock, state);
		return false;
	}

	src = rb->data + rb->head * rb->element_size;
	memcpy(out_element, src, rb->element_size);
	rb->head = (rb->head + 1u) % rb->capacity;
	rb->count--;
	spinlock_unlock_irqrestore(&rb->lock, state);
	return true;
}

bool ring_buffer_peek(struct ring_buffer* rb, void* out_element) {
	struct irq_state state;
	const uint8_t*   src;

	if (rb == NULL || out_element == NULL) return false;

	state = spinlock_lock_irqsave(&rb->lock);
	if (rb->count == 0u) {
		spinlock_unlock_irqrestore(&rb->lock, state);
		return false;
	}

	src = rb->data + rb->head * rb->element_size;
	memcpy(out_element, src, rb->element_size);
	spinlock_unlock_irqrestore(&rb->lock, state);
	return true;
}

bool ring_buffer_front_acquire(struct ring_buffer* rb, struct ring_buffer_front* out_front) {
	struct irq_state state;

	if (out_front != NULL) *out_front = (struct ring_buffer_front){0};
	if (rb == NULL || out_front == NULL) return false;

	state = spinlock_lock_irqsave(&rb->lock);
	if (rb->count == 0u) {
		spinlock_unlock_irqrestore(&rb->lock, state);
		return false;
	}

	out_front->buffer    = rb;
	out_front->element   = rb->data + rb->head * rb->element_size;
	out_front->irq_state = state;
	return true;
}

void ring_buffer_front_release(struct ring_buffer_front* front, bool consume) {
	struct ring_buffer* rb;
	struct irq_state    state;

	if (front == NULL || front->buffer == NULL) return;
	rb    = front->buffer;
	state = front->irq_state;
	if (consume) {
		rb->head = (rb->head + 1u) % rb->capacity;
		rb->count--;
	}
	front->buffer  = NULL;
	front->element = NULL;
	spinlock_unlock_irqrestore(&rb->lock, state);
}

size_t ring_buffer_count(struct ring_buffer* rb) {
	struct irq_state state;
	size_t           count;

	if (rb == NULL) return 0u;

	state = spinlock_lock_irqsave(&rb->lock);
	count = rb->count;
	spinlock_unlock_irqrestore(&rb->lock, state);
	return count;
}
