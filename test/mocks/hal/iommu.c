#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct mock_iommu_leaf {
	uint64_t  io_address;
	uintptr_t physical_address;
	size_t    size;
	uint64_t  access;
};

struct mock_iommu_source {
	uint32_t                      source_id;
	struct hal_iommu_space_state* space;
};

size_t hal_iommu_controller_count(void) {
	return 0u;
}

bool hal_iommu_controller_at(size_t index, struct hal_iommu_controller_descriptor* out_descriptor) {
	(void)index;
	(void)out_descriptor;
	return false;
}

static bool mock_power_of_two(size_t value) {
	return value != 0u && (value & (value - 1u)) == 0u;
}

static unsigned mock_shift(size_t value) {
	unsigned shift = 0u;
	while (((size_t)1u << shift) != value) shift++;
	return shift;
}

static bool mock_info_valid(const struct hal_iommu_info* info) {
	if (info == NULL || !mock_power_of_two(info->minimum_leaf_size) || info->source_id_bits > 32u ||
	    info->context_id_bits == 0u || info->context_id_bits > 32u || info->io_address_bits == 0u ||
	    info->io_address_bits >= 64u || info->physical_address_bits == 0u || info->physical_address_bits >= 64u)
		return false;
	unsigned shift = mock_shift(info->minimum_leaf_size);
	return shift < info->io_address_bits && shift < info->physical_address_bits &&
	       (info->leaf_size_mask & (1ull << shift)) != 0u && (info->leaf_size_mask & ((1ull << shift) - 1u)) == 0u;
}

static int mock_leaf_compare(const void* left, const void* right) {
	const struct mock_iommu_leaf* a = left;
	const struct mock_iommu_leaf* b = right;
	return a->io_address < b->io_address ? -1 : a->io_address > b->io_address;
}

static bool mock_range_end(uint64_t start, size_t size, uint8_t bits, uint64_t* out_end) {
	uint64_t limit = 1ull << bits;
	if (size == 0u || start > UINT64_MAX - size || start + size > limit) return false;
	*out_end = start + size;
	return true;
}

static bool mock_source_valid(const struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return controller != NULL && controller->initialized &&
	       (controller->info.source_id_bits == 32u || source_id < (1u << controller->info.source_id_bits));
}

static struct mock_iommu_source* mock_source_find(const struct hal_iommu_controller_state* controller,
                                                  uint32_t                                 source_id) {
	struct mock_iommu_source* sources = controller->source_entries;
	for (size_t index = 0u; index < controller->source_count; index++)
		if (sources[index].source_id == source_id) return &sources[index];
	return NULL;
}

bool hal_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                               const struct hal_iommu_controller_descriptor* descriptor,
                               struct hal_iommu_info*                        out_info) {
	if (controller == NULL || descriptor == NULL || out_info == NULL || !mock_info_valid(&descriptor->mock_info))
		return false;
	*controller           = (struct hal_iommu_controller_state){.initialized = true, .info = descriptor->mock_info};
	controller->info.kind = descriptor->kind;
	*out_info             = controller->info;
	return true;
}

void hal_iommu_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (controller == NULL || !controller->initialized || controller->live_spaces != 0u) return;
	if (controller->source_count != 0u) return;
	free(controller->source_entries);
	*controller = (struct hal_iommu_controller_state){0};
}

bool hal_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	return controller != NULL && controller->initialized && access != 0u &&
	       (access & ~HAL_IOMMU_ACCESS_VALID_MASK) == 0u;
}

bool hal_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                          struct hal_iommu_space_state* space) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    (controller->info.context_id_bits < 32u && context_id >= (1u << controller->info.context_id_bits)))
		return false;
	*space = (struct hal_iommu_space_state){
		.table = {.initialized = true, .controller_identity = (uintptr_t)controller, .context_id = context_id}
    };
	controller->live_spaces++;
	return true;
}

void hal_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	if (controller == NULL || space == NULL || !space->table.initialized || space->leaf_count != 0u ||
	    space->table.controller_identity != (uintptr_t)controller)
		return;
	for (size_t i = 0u; i < controller->source_count; i++)
		if (((struct mock_iommu_source*)controller->source_entries)[i].space == space) return;
	free(space->leaves);
	*space = (struct hal_iommu_space_state){0};
	controller->live_spaces--;
}

static size_t mock_largest_leaf(const struct hal_iommu_controller_state* controller, uint64_t io_address,
                                uintptr_t physical_address, size_t remaining) {
	for (unsigned shift = 63u; shift != 0u; shift--) {
		if ((controller->info.leaf_size_mask & (1ull << shift)) == 0u || shift >= sizeof(size_t) * 8u) continue;
		size_t size = (size_t)1u << shift;
		if ((io_address & (size - 1u)) == 0u && (physical_address & (size - 1u)) == 0u && remaining >= size)
			return size;
	}
	return controller->info.minimum_leaf_size;
}

bool hal_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request) {
	uint64_t end;
	uint64_t physical_end;
	if (!hal_iommu_mapping_supported(controller, request == NULL ? 0u : request->access) || space == NULL ||
	    request == NULL || !space->table.initialized || space->table.controller_identity != (uintptr_t)controller ||
	    (request->io_address & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    (request->physical_address & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    (request->size & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    !mock_range_end(request->io_address, request->size, controller->info.io_address_bits, &end) ||
	    !mock_range_end(
			request->physical_address, request->size, controller->info.physical_address_bits, &physical_end))
		return false;
	(void)physical_end;
	struct mock_iommu_leaf* old = space->leaves;
	for (size_t i = 0u; i < space->leaf_count; i++) {
		uint64_t leaf_end = old[i].io_address + old[i].size;
		if (request->io_address < leaf_end && old[i].io_address < end) return false;
	}
	size_t additions = 0u;
	for (uint64_t io = request->io_address; io < end;) {
		size_t leaf_size = mock_largest_leaf(
			controller, io, request->physical_address + (uintptr_t)(io - request->io_address), (size_t)(end - io));
		additions++;
		io += leaf_size;
	}
	if (additions > SIZE_MAX - space->leaf_count || space->leaf_count + additions > SIZE_MAX / sizeof(*old))
		return false;
	struct mock_iommu_leaf* replacement = malloc((space->leaf_count + additions) * sizeof(*replacement));
	if (replacement == NULL) return false;
	if (space->leaf_count != 0u) memcpy(replacement, old, space->leaf_count * sizeof(*replacement));
	size_t count = space->leaf_count;
	for (uint64_t io = request->io_address; io < end;) {
		uintptr_t physical   = request->physical_address + (uintptr_t)(io - request->io_address);
		size_t    leaf_size  = mock_largest_leaf(controller, io, physical, (size_t)(end - io));
		replacement[count++] = (struct mock_iommu_leaf){io, physical, leaf_size, request->access};
		io += leaf_size;
	}
	qsort(replacement, count, sizeof(*replacement), mock_leaf_compare);
	free(old);
	space->leaves     = replacement;
	space->leaf_count = count;
	space->table.mapped_size += request->size;
	controller->invalidations++;
	return true;
}

bool hal_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     uint64_t io_address, size_t size) {
	uint64_t end;
	if (controller == NULL || !controller->initialized || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)controller ||
	    (io_address & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    (size & (controller->info.minimum_leaf_size - 1u)) != 0u ||
	    !mock_range_end(io_address, size, controller->info.io_address_bits, &end))
		return false;
	struct mock_iommu_leaf* old    = space->leaves;
	uint64_t                cursor = io_address;
	for (size_t i = 0u; i < space->leaf_count && cursor < end; i++) {
		uint64_t leaf_end = old[i].io_address + old[i].size;
		if (leaf_end <= cursor) continue;
		if (old[i].io_address > cursor) return false;
		cursor = leaf_end < end ? leaf_end : end;
	}
	if (cursor != end) return false;
	size_t granule = controller->info.minimum_leaf_size;
	size_t maximum = space->table.mapped_size / granule;
	if (maximum > SIZE_MAX / sizeof(struct mock_iommu_leaf)) return false;
	struct mock_iommu_leaf* replacement = maximum == 0u ? NULL : malloc(maximum * sizeof(*replacement));
	if (maximum != 0u && replacement == NULL) return false;
	size_t count = 0u;
	for (size_t i = 0u; i < space->leaf_count; i++) {
		uint64_t leaf_start = old[i].io_address;
		uint64_t leaf_end   = leaf_start + old[i].size;
		if (leaf_end <= io_address || leaf_start >= end) {
			replacement[count++] = old[i];
			continue;
		}
		for (uint64_t part = leaf_start; part < io_address && part < leaf_end; part += granule)
			replacement[count++] = (struct mock_iommu_leaf){
				part, old[i].physical_address + (uintptr_t)(part - leaf_start), granule, old[i].access};
		for (uint64_t part = end > leaf_start ? end : leaf_start; part < leaf_end; part += granule)
			replacement[count++] = (struct mock_iommu_leaf){
				part, old[i].physical_address + (uintptr_t)(part - leaf_start), granule, old[i].access};
	}
	free(old);
	space->leaves     = replacement;
	space->leaf_count = count;
	space->table.mapped_size -= size;
	controller->invalidations++;
	return true;
}

bool hal_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                      uint32_t source_id) {
	if (!mock_source_valid(controller, source_id) || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	struct mock_iommu_source* source = mock_source_find(controller, source_id);
	if (source != NULL) return source->space == space;
	if (controller->source_count == controller->source_capacity) {
		size_t capacity = controller->source_capacity == 0u ? 8u : controller->source_capacity * 2u;
		if (capacity < controller->source_capacity || capacity > SIZE_MAX / sizeof(*source)) return false;
		struct mock_iommu_source* sources = realloc(controller->source_entries, capacity * sizeof(*sources));
		if (sources == NULL) return false;
		controller->source_entries  = sources;
		controller->source_capacity = capacity;
	}
	((struct mock_iommu_source*)controller->source_entries)[controller->source_count++] =
		(struct mock_iommu_source){.source_id = source_id, .space = space};
	controller->invalidations++;
	return true;
}

bool hal_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	if (!mock_source_valid(controller, source_id)) return false;
	struct mock_iommu_source* source = mock_source_find(controller, source_id);
	if (source == NULL) return false;
	*source = ((struct mock_iommu_source*)controller->source_entries)[--controller->source_count];
	controller->invalidations++;
	return true;
}
