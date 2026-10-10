#include <hal/device_tree.h>
#include <stddef.h>

#include "interrupts/controllers.h"

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	return loongarch64_device_tree_consumed_nodes(nodes, capacity);
}

bool hal_device_tree_interrupt_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
                                         size_t cell_count, uint32_t* out_local_source_id,
                                         enum hal_interrupt_trigger*  out_trigger,
                                         enum hal_interrupt_polarity* out_polarity) {
	return controller != NULL && controller->kind == HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER &&
	       cell_count == controller->specifier_cells &&
	       loongarch64_device_tree_interrupt(cells, cell_count, out_local_source_id, out_trigger, out_polarity);
}
