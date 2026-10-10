#pragma once

#include <firmware/dt.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resolution supplied for a node consumed by the HAL. */
enum hal_device_tree_reference_kind {
	HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED = 0,
	HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER,
	HAL_DEVICE_TREE_REFERENCE_DMA_CONTROLLER,
};

/* One firmware node consumed by a platform HAL provider. */
struct hal_device_tree_consumed_node {
	struct dt_node                      node;
	enum hal_device_tree_reference_kind kind;
	uint64_t                            value;
	/* Zero when the reference cannot translate controller-specific specifiers. */
	uint32_t specifier_cells;
};

/* List the Device Tree nodes consumed by all platform HAL providers. */
size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);

/* Translate one interrupt-controller phandle specifier into the kernel's fixed-source model. */
bool hal_device_tree_interrupt_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
                                         size_t cell_count, uint32_t* out_local_source_id,
                                         enum hal_interrupt_trigger*  out_trigger,
                                         enum hal_interrupt_polarity* out_polarity);
