#include <hal/device_tree.h>
#include <stddef.h>

#include "interrupts/controllers.h"

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	return loongarch64_device_tree_consumed_nodes(nodes, capacity);
}
