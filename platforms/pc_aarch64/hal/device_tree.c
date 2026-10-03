#include <hal/device_tree.h>
#include <stddef.h>

#include "interrupts/gic.h"
#include "interrupts/gicv3.h"
#include "iommu/iommu.h"

static size_t device_tree_append(size_t (*provider)(struct hal_device_tree_consumed_node*, size_t),
                                 struct hal_device_tree_consumed_node* nodes, size_t capacity, size_t count) {
	size_t available = count < capacity ? capacity - count : 0u;
	size_t added     = provider(nodes != NULL && available != 0u ? nodes + count : NULL, available);

	return added > SIZE_MAX - count ? SIZE_MAX : count + added;
}

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	size_t count = 0u;

	count = device_tree_append(aarch64_gic_device_tree_consumed_nodes, nodes, capacity, count);
	count = device_tree_append(aarch64_gicv3_device_tree_consumed_nodes, nodes, capacity, count);
	count = device_tree_append(aarch64_iommu_device_tree_consumed_nodes, nodes, capacity, count);
	return count;
}
