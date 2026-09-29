#include <core/io_port.h>
#include <core/process.h>
#include <core/spinlock.h>
#include <hal/hcf.h>
#include <libc/stdlib.h>

struct io_port_mapping {
	struct io_port_mapping* next;
	cap_id_t                capability;
	uint32_t                base;
	uint32_t                count;
};

struct io_port_state {
	struct io_port_mapping*   mappings;
	struct hal_io_port_bitmap bitmap;
	uint64_t                  generation;
};

static bool io_port_range_valid(uint32_t base, uint32_t count) {
	return count != 0u && base < HAL_IO_PORT_COUNT && count <= HAL_IO_PORT_COUNT - base;
}

static void io_port_state_rebuild(struct io_port_state* state) {
	hal_io_port_bitmap_deny_all(&state->bitmap);
	for (struct io_port_mapping* mapping = state->mappings; mapping != NULL; mapping = mapping->next) {
		if (!hal_io_port_bitmap_allow(&state->bitmap, mapping->base, mapping->count)) hcf();
	}
}

static void io_port_state_free(struct io_port_state* state) {
	if (state == NULL) return;
	while (state->mappings != NULL) {
		struct io_port_mapping* mapping = state->mappings;
		state->mappings                 = mapping->next;
		free(mapping);
	}
	free(state);
}

enum io_port_result io_port_map(struct process* process, cap_id_t capability, uint32_t base, uint32_t count) {
	struct io_port_mapping* mapping;
	struct io_port_state*   allocated_state = NULL;
	struct io_port_state*   state;
	struct irq_state        irq;

	if (process == NULL || capability == CAP_ID_INVALID || !io_port_range_valid(base, count))
		return IO_PORT_INVALID_ARGUMENTS;
	mapping = malloc(sizeof(*mapping));
	if (mapping == NULL) return IO_PORT_NO_MEMORY;

	irq   = spinlock_lock_irqsave(&process->lock);
	state = process->io_port_state;
	if (state == NULL) {
		spinlock_unlock_irqrestore(&process->lock, irq);
		allocated_state = calloc(1u, sizeof(*allocated_state));
		if (allocated_state == NULL) {
			free(mapping);
			return IO_PORT_NO_MEMORY;
		}
		hal_io_port_bitmap_deny_all(&allocated_state->bitmap);
		irq   = spinlock_lock_irqsave(&process->lock);
		state = process->io_port_state;
	}
	if (process->state != PROCESS_STATE_NEW && process->state != PROCESS_STATE_RUNNING) {
		spinlock_unlock_irqrestore(&process->lock, irq);
		free(allocated_state);
		free(mapping);
		return IO_PORT_INVALID_ARGUMENTS;
	}
	if (state == NULL) {
		state                  = allocated_state;
		allocated_state        = NULL;
		process->io_port_state = state;
	}
	for (struct io_port_mapping* current = state->mappings; current != NULL; current = current->next) {
		if (current->capability == capability) {
			spinlock_unlock_irqrestore(&process->lock, irq);
			free(allocated_state);
			free(mapping);
			return IO_PORT_ALREADY_MAPPED;
		}
	}
	*mapping = (struct io_port_mapping){
		.next       = state->mappings,
		.capability = capability,
		.base       = base,
		.count      = count,
	};
	state->mappings = mapping;
	if (!hal_io_port_bitmap_allow(&state->bitmap, base, count)) hcf();
	if (++state->generation == 0u) state->generation = 1u;
	spinlock_unlock_irqrestore(&process->lock, irq);
	free(allocated_state);
	return IO_PORT_OK;
}

enum io_port_result io_port_unmap(struct process* process, cap_id_t capability) {
	struct io_port_mapping** link;
	struct io_port_mapping*  removed;
	struct io_port_state*    detached = NULL;
	struct io_port_state*    state;
	struct irq_state         irq;

	if (process == NULL || capability == CAP_ID_INVALID) return IO_PORT_INVALID_ARGUMENTS;
	irq   = spinlock_lock_irqsave(&process->lock);
	state = process->io_port_state;
	if (state == NULL) {
		spinlock_unlock_irqrestore(&process->lock, irq);
		return IO_PORT_NOT_MAPPED;
	}
	link = &state->mappings;
	while (*link != NULL && (*link)->capability != capability) link = &(*link)->next;
	if (*link == NULL) {
		spinlock_unlock_irqrestore(&process->lock, irq);
		return IO_PORT_NOT_MAPPED;
	}
	removed = *link;
	*link   = removed->next;
	if (state->mappings == NULL) {
		process->io_port_state = NULL;
		detached               = state;
	}
	else {
		io_port_state_rebuild(state);
		if (++state->generation == 0u) state->generation = 1u;
	}
	spinlock_unlock_irqrestore(&process->lock, irq);
	free(removed);
	hal_io_port_bitmap_invalidate_all();
	io_port_state_free(detached);
	return IO_PORT_OK;
}

bool io_port_unmap_capability(process_id_t process_id, cap_id_t capability) {
	struct process* process = process_acquire(process_id);
	bool            result;
	if (process == NULL) return false;
	result = io_port_unmap(process, capability) == IO_PORT_OK;
	process_release(process);
	return result;
}

void io_port_process_deinit(struct process* process) {
	struct io_port_state* state;
	struct irq_state      irq;
	if (process == NULL) return;
	irq                    = spinlock_lock_irqsave(&process->lock);
	state                  = process->io_port_state;
	process->io_port_state = NULL;
	spinlock_unlock_irqrestore(&process->lock, irq);
	if (state == NULL) return;
	hal_io_port_bitmap_invalidate_all();
	io_port_state_free(state);
}

void io_port_process_load(struct process* process) {
	struct io_port_state* state;
	struct irq_state      irq;

	if (process == NULL) {
		if (!hal_io_port_bitmap_select(NULL, 0u)) hcf();
		return;
	}
	irq   = spinlock_lock_irqsave(&process->lock);
	state = process->io_port_state;
	if (state == NULL) {
		if (!hal_io_port_bitmap_select(NULL, 0u)) hcf();
	}
	else if (!hal_io_port_bitmap_select(&state->bitmap, state->generation)) {
		hcf();
	}
	spinlock_unlock_irqrestore(&process->lock, irq);
}

size_t io_port_process_mapping_count(struct process* process) {
	struct io_port_state* state;
	struct irq_state      irq;
	size_t                count = 0u;
	if (process == NULL) return 0u;
	irq   = spinlock_lock_irqsave(&process->lock);
	state = process->io_port_state;
	if (state != NULL)
		for (struct io_port_mapping* mapping = state->mappings; mapping != NULL; mapping = mapping->next) count++;
	spinlock_unlock_irqrestore(&process->lock, irq);
	return count;
}

uint64_t io_port_process_generation(struct process* process) {
	struct irq_state irq;
	uint64_t         generation;
	if (process == NULL) return 0u;
	irq        = spinlock_lock_irqsave(&process->lock);
	generation = process->io_port_state == NULL ? 0u : process->io_port_state->generation;
	spinlock_unlock_irqrestore(&process->lock, irq);
	return generation;
}
