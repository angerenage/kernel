#include <stddef.h>

#include <hal/device_tree.h>

#include "interrupts/controllers.h"
#include "serial.h"

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	size_t count = loongarch64_device_tree_consumed_nodes(nodes, capacity);
	size_t available = count < capacity ? capacity - count : 0u;
	size_t added = loongarch64_serial_device_tree_consumed_nodes(
			nodes != NULL && available != 0u ? nodes + count : NULL, available);
	return added > SIZE_MAX - count ? SIZE_MAX : count + added;
}

bool hal_device_tree_interrupt_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
		size_t cell_count, uint32_t* out_local_source_id, enum hal_interrupt_trigger* out_trigger,
		enum hal_interrupt_polarity* out_polarity) {
	return controller != NULL && controller->kind == HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER &&
	       cell_count == controller->specifier_cells &&
	       loongarch64_device_tree_interrupt(cells, cell_count, out_local_source_id, out_trigger, out_polarity);
}

bool hal_device_tree_dma_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
		size_t cell_count, uint32_t* out_local_source_id) {
	(void)controller;
	(void)cells;
	(void)cell_count;
	(void)out_local_source_id;
	return false;
}
