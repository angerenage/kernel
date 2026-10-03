#pragma once

#include <firmware/dt.h>
#include <stddef.h>
#include <stdint.h>

/* Resolution supplied for a node consumed by the HAL. */
enum hal_device_tree_reference_kind {
	HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED = 0,
	HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER,
	HAL_DEVICE_TREE_REFERENCE_DMA_CONTROLLER,
};

struct hal_device_tree_consumed_node {
	struct dt_node                      node;
	enum hal_device_tree_reference_kind kind;
	uint64_t                            value;
};

/* List the Device Tree nodes consumed by all platform HAL providers. */
size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity);
