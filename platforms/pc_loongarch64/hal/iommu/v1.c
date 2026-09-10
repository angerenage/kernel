#include "v1.h"

#include <core/mm.h>
#include <hal/hcf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../iommu_page_table.h"

#define LA_IOMMU_VBTC 0x014u
#define LA_IOMMU_EIVDB 0x018u
#define LA_IOMMU_COMMAND 0x01cu
#define LA_IOMMU_PGD_LOW(context) (0x020u + (size_t)(context) * 8u)
#define LA_IOMMU_PGD_HIGH(context) (0x024u + (size_t)(context) * 8u)
#define LA_IOMMU_DIRECTORY_CONTROL(context) (0x0a0u + (size_t)(context) * 4u)
#define LA_IOMMU_ENABLE 0x100u
#define LA_IOMMU_ENABLE_TRANSLATION (1u << 31u)
#define LA_IOMMU_USE_MEMORY (1u << 29u)
#define LA_IOMMU_VBTC_BUSY (1u << 16u)
#define LA_IOMMU_PAGE_SHIFT 14u
#define LA_IOMMU_INDEX_BITS 11u
#define LA_IOMMU_LEVELS 3u
#define LA_IOMMU_SLOT_COUNT 16u
#define LA_IOMMU_WAIT_LIMIT 1000000u

static void la_lock(struct hal_iommu_controller_state* controller) {
	while (__atomic_exchange_n(&controller->operation_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
		__asm__ volatile("nop" : : : "memory");
}

static void la_unlock(struct hal_iommu_controller_state* controller) {
	__atomic_store_n(&controller->operation_lock, 0u, __ATOMIC_RELEASE);
}

static inline volatile uint8_t* la_registers(const struct hal_iommu_controller_state* controller) {
	return (volatile uint8_t*)controller->registers;
}

static inline uint32_t la_read32(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint32_t*)(la_registers(controller) + offset);
}

static inline void la_write32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(la_registers(controller) + offset) = value;
}

static bool la_wait_vbtc(const struct hal_iommu_controller_state* controller) {
	for (size_t attempt = 0u; attempt < LA_IOMMU_WAIT_LIMIT; attempt++) {
		if ((la_read32(controller, LA_IOMMU_VBTC) & LA_IOMMU_VBTC_BUSY) == 0u) return true;
		__asm__ volatile("nop" : : : "memory");
	}
	return false;
}

static struct iommu_pt_format la_format(const struct hal_iommu_controller_state* controller) {
	return (struct iommu_pt_format){.kind                  = IOMMU_PT_LOONGARCH_V1,
	                                .address_mask          = controller->address_mask,
	                                .leaf_size_mask        = controller->leaf_size_mask,
	                                .levels                = LA_IOMMU_LEVELS,
	                                .io_address_bits       = controller->io_address_bits,
	                                .physical_address_bits = controller->physical_address_bits,
	                                .page_shift            = LA_IOMMU_PAGE_SHIFT,
	                                .index_bits            = LA_IOMMU_INDEX_BITS};
}

static bool la_sync(void* raw_controller, uint32_t context_id, uint64_t io_address, size_t size,
                    bool hierarchy_changed) {
	struct hal_iommu_controller_state* controller = raw_controller;
	(void)io_address;
	(void)size;
	(void)hierarchy_changed;
	uint32_t eivdb = la_read32(controller, LA_IOMMU_EIVDB);
	la_write32(controller, LA_IOMMU_EIVDB, (eivdb & ~(0xfu << 16u)) | ((context_id & 0xfu) << 16u));
	la_write32(controller, LA_IOMMU_VBTC, (la_read32(controller, LA_IOMMU_VBTC) & ~0x10fu) | 4u);
	if (!la_wait_vbtc(controller)) return false;
	la_write32(controller, LA_IOMMU_VBTC, (la_read32(controller, LA_IOMMU_VBTC) & ~0x10fu) | (1u << 8u) | 4u);
	return la_wait_vbtc(controller);
}

static void la_set_translation(const struct hal_iommu_controller_state* controller, bool enabled) {
	uint32_t value = la_read32(controller, LA_IOMMU_ENABLE) | LA_IOMMU_USE_MEMORY;
	if (enabled) value |= LA_IOMMU_ENABLE_TRANSLATION;
	else value &= ~LA_IOMMU_ENABLE_TRANSLATION;
	la_write32(controller, LA_IOMMU_ENABLE, value);
	la_write32(controller, LA_IOMMU_COMMAND, la_read32(controller, LA_IOMMU_COMMAND) & ~3u);
}

bool loongarch_iommu_v1_controller_init(struct hal_iommu_controller_state*            controller,
                                        const struct hal_iommu_controller_descriptor* descriptor,
                                        struct hal_iommu_info*                        out_info) {
	if (controller == NULL || descriptor == NULL || out_info == NULL ||
	    descriptor->kind != HAL_IOMMU_KIND_LOONGARCH_V1 || descriptor->register_address == 0u ||
	    descriptor->register_size < 0x104u || descriptor->firmware_page_table_levels < LA_IOMMU_LEVELS ||
	    descriptor->firmware_physical_address_bits < LA_IOMMU_PAGE_SHIFT ||
	    descriptor->firmware_physical_address_bits >= 64u || descriptor->firmware_io_address_bits < 36u ||
	    descriptor->firmware_io_address_bits >= 64u || descriptor->maximum_device_count == 0u ||
	    !iommu_pt_map_mmio(descriptor->register_address, descriptor->register_size))
		return false;
	uint8_t physical_bits =
		descriptor->firmware_physical_address_bits > 48u ? 48u : (uint8_t)descriptor->firmware_physical_address_bits;
	uint8_t  io_bits = descriptor->firmware_io_address_bits > 47u ? 47u : (uint8_t)descriptor->firmware_io_address_bits;
	uint64_t leaves  = descriptor->firmware_leaf_size_mask & ((1ull << 14u) | (1ull << 25u));
	if ((leaves & (1ull << LA_IOMMU_PAGE_SHIFT)) == 0u) return false;
	*controller = (struct hal_iommu_controller_state){
		.registers             = iommu_pt_phys_to_virt(descriptor->register_address),
		.leaf_size_mask        = leaves,
		.address_mask          = ((1ull << physical_bits) - 1u) & ~((1ull << LA_IOMMU_PAGE_SHIFT) - 1u),
		.io_address_bits       = io_bits,
		.physical_address_bits = physical_bits,
	};
	la_set_translation(controller, false);
	la_write32(controller, LA_IOMMU_VBTC, (la_read32(controller, LA_IOMMU_VBTC) & ~0x10fu) | 5u);
	if (!la_wait_vbtc(controller)) return false;
	controller->initialized = true;
	*out_info               = (struct hal_iommu_info){.kind                  = HAL_IOMMU_KIND_LOONGARCH_V1,
	                                                  .minimum_leaf_size     = 1u << LA_IOMMU_PAGE_SHIFT,
	                                                  .leaf_size_mask        = leaves,
	                                                  .io_address_bits       = io_bits,
	                                                  .physical_address_bits = physical_bits,
	                                                  .context_id_bits       = 4u,
	                                                  .source_id_bits        = 16u};
	return true;
}

void loongarch_iommu_v1_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (controller == NULL || !controller->initialized || controller->allocated_slots != 0u) return;
	la_lock(controller);
	la_set_translation(controller, false);
	la_write32(controller, LA_IOMMU_VBTC, (la_read32(controller, LA_IOMMU_VBTC) & ~0x10fu) | 5u);
	if (!la_wait_vbtc(controller)) hcf();
	controller->initialized = false;
	la_unlock(controller);
}

bool loongarch_iommu_v1_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	if (controller == NULL || !controller->initialized) return false;
	struct iommu_pt_format format = la_format(controller);
	return iommu_pt_access_supported(&format, access);
}

bool loongarch_iommu_v1_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                                   struct hal_iommu_space_state* space) {
	if (controller == NULL || !controller->initialized || context_id >= 16u) return false;
	la_lock(controller);
	struct iommu_pt_format format = la_format(controller);
	bool                   result = iommu_pt_space_init(&format, (uintptr_t)controller, context_id, space);
	la_unlock(controller);
	return result;
}

void loongarch_iommu_v1_space_deinit(struct hal_iommu_controller_state* controller,
                                     struct hal_iommu_space_state*      space) {
	if (controller == NULL || space == NULL || space->table.controller_identity != (uintptr_t)controller) return;
	la_lock(controller);
	struct iommu_pt_format format = la_format(controller);
	iommu_pt_space_deinit(&format, space);
	la_unlock(controller);
}

bool loongarch_iommu_v1_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                            const struct hal_iommu_map_request* request) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	la_lock(controller);
	struct iommu_pt_format format = la_format(controller);
	bool                   result = iommu_pt_map(&format, space, request, la_sync, controller);
	la_unlock(controller);
	return result;
}

bool loongarch_iommu_v1_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                              uint64_t io_address, size_t size) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	la_lock(controller);
	struct iommu_pt_format format = la_format(controller);
	bool                   result = iommu_pt_unmap(&format, space, io_address, size, la_sync, controller);
	la_unlock(controller);
	return result;
}

bool loongarch_iommu_v1_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                               uint32_t source_id, struct hal_iommu_attachment_state* attachment) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller || source_id > UINT16_MAX || attachment == NULL ||
	    attachment->initialized)
		return false;
	la_lock(controller);
	size_t slot;
	for (slot = 0u; slot < LA_IOMMU_SLOT_COUNT; slot++)
		if ((controller->allocated_slots & (1u << slot)) == 0u) break;
	if (slot == LA_IOMMU_SLOT_COUNT) {
		la_unlock(controller);
		return false;
	}
	uint32_t context           = space->table.context_id;
	uint32_t directory_control = (LA_IOMMU_INDEX_BITS << 26u) | ((LA_IOMMU_PAGE_SHIFT + 22u) << 20u) |
	                             (LA_IOMMU_INDEX_BITS << 16u) | ((LA_IOMMU_PAGE_SHIFT + 11u) << 10u) |
	                             (LA_IOMMU_INDEX_BITS << 6u) | LA_IOMMU_PAGE_SHIFT;
	la_write32(controller, LA_IOMMU_DIRECTORY_CONTROL(context), directory_control);
	la_write32(controller, LA_IOMMU_PGD_HIGH(context), (uint32_t)(space->table.root_address >> 32u));
	la_write32(controller, LA_IOMMU_PGD_LOW(context), (uint32_t)space->table.root_address);
	la_write32(
		controller, LA_IOMMU_EIVDB, (uint32_t)source_id | (context << 16u) | (1u << 20u) | ((uint32_t)slot << 24u));
	la_set_translation(controller, true);
	if (!la_sync(controller, context, 0u, 0u, true)) hcf();
	controller->allocated_slots |= (uint16_t)(1u << slot);
	*attachment = (struct hal_iommu_attachment_state){.initialized         = true,
	                                                  .controller_identity = (uintptr_t)controller,
	                                                  .source_id           = source_id,
	                                                  .hardware_slot       = (uint8_t)slot,
	                                                  .context_id          = (uint8_t)context};
	la_unlock(controller);
	return true;
}

bool loongarch_iommu_v1_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                               struct hal_iommu_attachment_state* attachment) {
	if (controller == NULL || !controller->initialized || source_id > UINT16_MAX || attachment == NULL ||
	    !attachment->initialized || attachment->controller_identity != (uintptr_t)controller ||
	    attachment->source_id != source_id || attachment->hardware_slot >= LA_IOMMU_SLOT_COUNT)
		return false;
	la_lock(controller);
	size_t slot = attachment->hardware_slot;
	if ((controller->allocated_slots & (1u << slot)) == 0u) {
		la_unlock(controller);
		return false;
	}
	uint32_t context = attachment->context_id;
	la_write32(controller, LA_IOMMU_EIVDB, (uint32_t)slot << 24u);
	if (!la_sync(controller, context, 0u, 0u, true)) hcf();
	controller->allocated_slots &= (uint16_t)~(1u << slot);
	*attachment = (struct hal_iommu_attachment_state){0};
	la_unlock(controller);
	return true;
}
