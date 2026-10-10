#include <hal/device_tree.h>
#include <stddef.h>

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	(void)nodes;
	(void)capacity;
	return 0u;
}

bool hal_device_tree_interrupt_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
                                         size_t cell_count, uint32_t* out_local_source_id,
                                         enum hal_interrupt_trigger*  out_trigger,
                                         enum hal_interrupt_polarity* out_polarity) {
	(void)controller;
	(void)cells;
	(void)cell_count;
	(void)out_local_source_id;
	(void)out_trigger;
	(void)out_polarity;
	return false;
}

bool hal_device_tree_dma_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
                                   size_t cell_count, uint32_t* out_local_source_id) {
	(void)controller;
	(void)cells;
	(void)cell_count;
	(void)out_local_source_id;
	return false;
}
