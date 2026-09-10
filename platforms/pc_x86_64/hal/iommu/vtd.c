#include "vtd.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <hal/hcf.h>
#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_page_table.h"

#define VTD_REG_VERSION 0x000u
#define VTD_REG_CAPABILITY 0x008u
#define VTD_REG_EXTENDED_CAPABILITY 0x010u
#define VTD_REG_GLOBAL_COMMAND 0x018u
#define VTD_REG_GLOBAL_STATUS 0x01cu
#define VTD_REG_ROOT_TABLE_ADDRESS 0x020u
#define VTD_REG_CONTEXT_COMMAND 0x028u
#define VTD_REGISTER_SPACE_SIZE 0x4000u
#define VTD_GLOBAL_TRANSLATION_ENABLE (1u << 31u)
#define VTD_GLOBAL_SET_ROOT_TABLE_POINTER (1u << 30u)
#define VTD_CONTEXT_INVALIDATE (1ull << 63u)
#define VTD_CONTEXT_GLOBAL (1ull << 61u)
#define VTD_IOTLB_INVALIDATE (1ull << 63u)
#define VTD_IOTLB_GLOBAL (1ull << 60u)
#define VTD_IOTLB_DRAIN_READS (1ull << 49u)
#define VTD_IOTLB_DRAIN_WRITES (1ull << 48u)
#define VTD_CAP_DRAIN_READS (1ull << 55u)
#define VTD_CAP_DRAIN_WRITES (1ull << 54u)
#define VTD_WAIT_LIMIT 1000000u

static void vtd_lock(struct hal_iommu_controller_state* controller) {
	while (__atomic_exchange_n(&controller->operation_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
		__asm__ volatile("pause" : : : "memory");
}

static void vtd_unlock(struct hal_iommu_controller_state* controller) {
	__atomic_store_n(&controller->operation_lock, 0u, __ATOMIC_RELEASE);
}

static inline volatile uint8_t* vtd_registers(const struct hal_iommu_controller_state* controller) {
	return (volatile uint8_t*)controller->registers;
}

static inline uint32_t vtd_read32(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint32_t*)(vtd_registers(controller) + offset);
}

static inline uint64_t vtd_read64(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint64_t*)(vtd_registers(controller) + offset);
}

static inline void vtd_write32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(vtd_registers(controller) + offset) = value;
}

static inline void vtd_write64(const struct hal_iommu_controller_state* controller, size_t offset, uint64_t value) {
	*(volatile uint64_t*)(vtd_registers(controller) + offset) = value;
}

static bool vtd_wait32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t mask, bool set) {
	for (size_t attempt = 0u; attempt < VTD_WAIT_LIMIT; attempt++) {
		if (((vtd_read32(controller, offset) & mask) != 0u) == set) return true;
		__asm__ volatile("pause" : : : "memory");
	}
	return false;
}

static bool vtd_wait64(const struct hal_iommu_controller_state* controller, size_t offset, uint64_t mask, bool set) {
	for (size_t attempt = 0u; attempt < VTD_WAIT_LIMIT; attempt++) {
		if (((vtd_read64(controller, offset) & mask) != 0u) == set) return true;
		__asm__ volatile("pause" : : : "memory");
	}
	return false;
}

static inline uint64_t* vtd_root_virt(const struct hal_iommu_controller_state* controller) {
	return (uint64_t*)iommu_pt_phys_to_virt(controller->source_table_address);
}

static inline bool vtd_source_valid(const struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return controller != NULL && controller->initialized &&
	       (controller->source_id_bits == 32u || source_id < (1u << controller->source_id_bits));
}

static struct iommu_pt_format vtd_format(const struct hal_iommu_controller_state* controller) {
	return (struct iommu_pt_format){.kind                  = IOMMU_PT_INTEL_VTD,
	                                .address_mask          = controller->address_mask,
	                                .leaf_size_mask        = controller->leaf_size_mask,
	                                .levels                = controller->page_table_levels,
	                                .io_address_bits       = controller->io_address_bits,
	                                .physical_address_bits = controller->physical_address_bits};
}

static bool vtd_context_invalidate(struct hal_iommu_controller_state* controller) {
	vtd_write64(controller, VTD_REG_CONTEXT_COMMAND, VTD_CONTEXT_INVALIDATE | VTD_CONTEXT_GLOBAL);
	return vtd_wait64(controller, VTD_REG_CONTEXT_COMMAND, VTD_CONTEXT_INVALIDATE, false);
}

static bool vtd_iotlb_invalidate(struct hal_iommu_controller_state* controller) {
	size_t   offset  = (size_t)((controller->extended_capabilities >> 8u) & 0x3ffu) * 16u + 8u;
	uint64_t command = VTD_IOTLB_INVALIDATE | VTD_IOTLB_GLOBAL;
	if ((controller->capabilities & VTD_CAP_DRAIN_READS) != 0u) command |= VTD_IOTLB_DRAIN_READS;
	if ((controller->capabilities & VTD_CAP_DRAIN_WRITES) != 0u) command |= VTD_IOTLB_DRAIN_WRITES;
	vtd_write64(controller, offset, command);
	return vtd_wait64(controller, offset, VTD_IOTLB_INVALIDATE, false);
}

static bool vtd_sync(void* context, uint32_t context_id, uint64_t io_address, size_t size, bool hierarchy_changed) {
	struct hal_iommu_controller_state* controller = context;
	(void)context_id;
	(void)io_address;
	(void)size;
	return (!hierarchy_changed || vtd_context_invalidate(controller)) && vtd_iotlb_invalidate(controller);
}

static bool vtd_root_empty(const struct hal_iommu_controller_state* controller) {
	const uint64_t* root = vtd_root_virt(controller);
	for (size_t index = 0u; index < 256u; index++)
		if ((root[index * 2u] & 1u) != 0u) return false;
	return true;
}

static bool vtd_set_translation(struct hal_iommu_controller_state* controller, bool enabled) {
	if (enabled) controller->command |= VTD_GLOBAL_TRANSLATION_ENABLE;
	else controller->command &= ~VTD_GLOBAL_TRANSLATION_ENABLE;
	vtd_write32(controller, VTD_REG_GLOBAL_COMMAND, controller->command);
	return vtd_wait32(controller, VTD_REG_GLOBAL_STATUS, VTD_GLOBAL_TRANSLATION_ENABLE, enabled);
}

static bool vtd_initialize(struct hal_iommu_controller_state*            controller,
                           const struct hal_iommu_controller_descriptor* descriptor, struct hal_iommu_info* out_info) {
	const struct pmm_info* pmm = pmm_info();
	struct pmm_extent      root;
	uint64_t               capability;
	uint64_t               extended;
	uint32_t               sagaw;
	unsigned               agaw;
	unsigned               domain_encoding;
	unsigned               physical_bits;
	uint32_t               version;

	if (descriptor->register_address == 0u || pmm == NULL || pmm->allocation_granule == 0u ||
	    (pmm->allocation_granule & (pmm->allocation_granule - 1u)) != 0u)
		return false;
	if (!iommu_pt_map_mmio(descriptor->register_address, VTD_REGISTER_SPACE_SIZE)) return false;
	*controller = (struct hal_iommu_controller_state){
		.kind      = HAL_IOMMU_KIND_INTEL_VTD,
		.registers = iommu_pt_phys_to_virt(descriptor->register_address),
		.table_allocation_size =
			pmm->allocation_granule > IOMMU_PT_PAGE_SIZE ? pmm->allocation_granule : IOMMU_PT_PAGE_SIZE};
	version    = vtd_read32(controller, VTD_REG_VERSION);
	capability = vtd_read64(controller, VTD_REG_CAPABILITY);
	extended   = vtd_read64(controller, VTD_REG_EXTENDED_CAPABILITY);
	if ((version >> 4u) == 0u) return false;
	sagaw = (uint32_t)((capability >> 8u) & 0x1fu);
	for (agaw = 4u; agaw != 0u; agaw--)
		if ((sagaw & (1u << (agaw - 1u))) != 0u) break;
	if (agaw == 0u) return false;
	agaw--;
	physical_bits = descriptor->firmware_physical_address_bits;
	if (physical_bits < IOMMU_PT_PAGE_SHIFT || physical_bits > 52u) return false;
	domain_encoding = (unsigned)(capability & 0x7u);
	if (domain_encoding > 6u) return false;
	controller->capabilities          = capability;
	controller->extended_capabilities = extended;
	unsigned adjusted_guest_bits      = 30u + 9u * agaw;
	unsigned maximum_guest_bits       = (unsigned)((capability >> 16u) & 0x3fu) + 1u;
	if (maximum_guest_bits < 30u) return false;
	controller->io_address_bits =
		(uint8_t)(maximum_guest_bits < adjusted_guest_bits ? maximum_guest_bits : adjusted_guest_bits);
	controller->physical_address_bits = (uint8_t)physical_bits;
	controller->page_table_levels     = (uint8_t)(2u + agaw);
	controller->context_id_bits       = (uint8_t)(4u + 2u * domain_encoding);
	if (controller->context_id_bits > 16u) controller->context_id_bits = 16u;
	controller->context_limit =
		controller->context_id_bits == 16u ? UINT16_MAX : (uint16_t)((1u << controller->context_id_bits) - 1u);
	controller->source_id_bits = 16u;
	controller->address_mask   = ((1ull << physical_bits) - 1u) & ~(uint64_t)(IOMMU_PT_PAGE_SIZE - 1u);
	controller->leaf_size_mask = 1ull << IOMMU_PT_PAGE_SHIFT;
	uint64_t superpages        = (capability >> 34u) & 0xfu;
	if ((superpages & 1u) != 0u && controller->io_address_bits >= 30u) controller->leaf_size_mask |= 1ull << 21u;
	if ((superpages & 2u) != 0u && controller->io_address_bits >= 39u) controller->leaf_size_mask |= 1ull << 30u;
	if (!iommu_pt_allocate_table(controller->table_allocation_size, &root)) return false;
	if (((uint64_t)root.address & ~controller->address_mask) != 0u) {
		(void)pmm_free(root);
		return false;
	}
	controller->source_table_address = root.address;
	controller->command              = vtd_read32(controller, VTD_REG_GLOBAL_STATUS) & VTD_GLOBAL_TRANSLATION_ENABLE;
	if (!vtd_set_translation(controller, false)) {
		(void)pmm_free(root);
		controller->source_table_address = 0u;
		return false;
	}
	vtd_write64(controller, VTD_REG_ROOT_TABLE_ADDRESS, root.address);
	vtd_write32(controller, VTD_REG_GLOBAL_COMMAND, controller->command | VTD_GLOBAL_SET_ROOT_TABLE_POINTER);
	if (!vtd_wait32(controller, VTD_REG_GLOBAL_STATUS, VTD_GLOBAL_SET_ROOT_TABLE_POINTER, true) ||
	    !vtd_context_invalidate(controller) || !vtd_iotlb_invalidate(controller) ||
	    !vtd_set_translation(controller, true))
		hcf();
	controller->initialized = true;
	*out_info               = (struct hal_iommu_info){.kind                  = HAL_IOMMU_KIND_INTEL_VTD,
	                                                  .minimum_leaf_size     = IOMMU_PT_PAGE_SIZE,
	                                                  .leaf_size_mask        = controller->leaf_size_mask,
	                                                  .io_address_bits       = controller->io_address_bits,
	                                                  .physical_address_bits = controller->physical_address_bits,
	                                                  .context_id_bits       = controller->context_id_bits,
	                                                  .source_id_bits        = controller->source_id_bits};
	return true;
}

bool x86_vtd_controller_init(struct hal_iommu_controller_state*            controller,
                             const struct hal_iommu_controller_descriptor* descriptor,
                             struct hal_iommu_info*                        out_info) {
	if (controller == NULL || descriptor == NULL || out_info == NULL || descriptor->kind != HAL_IOMMU_KIND_INTEL_VTD)
		return false;
	return vtd_initialize(controller, descriptor, out_info);
}

void x86_vtd_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (controller == NULL || !controller->initialized || controller->kind != HAL_IOMMU_KIND_INTEL_VTD) return;
	vtd_lock(controller);
	if (!vtd_root_empty(controller)) {
		vtd_unlock(controller);
		return;
	}
	if (!vtd_set_translation(controller, false)) hcf();
	(void)pmm_free(
		(struct pmm_extent){.address = controller->source_table_address, .size = controller->table_allocation_size});
	controller->initialized = false;
	vtd_unlock(controller);
}

bool x86_vtd_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	if (controller == NULL || !controller->initialized || controller->kind != HAL_IOMMU_KIND_INTEL_VTD) return false;
	struct iommu_pt_format format = vtd_format(controller);
	return iommu_pt_access_supported(&format, access);
}

bool x86_vtd_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                        struct hal_iommu_space_state* space) {
	if (controller == NULL || !controller->initialized || space == NULL || context_id > controller->context_limit)
		return false;
	vtd_lock(controller);
	struct iommu_pt_format format = vtd_format(controller);
	bool                   ok     = iommu_pt_space_init(&format, (uintptr_t)controller, context_id, space);
	vtd_unlock(controller);
	return ok;
}

void x86_vtd_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	if (controller == NULL || space == NULL || space->table.controller_identity != (uintptr_t)controller) return;
	vtd_lock(controller);
	struct iommu_pt_format format = vtd_format(controller);
	iommu_pt_space_deinit(&format, space);
	vtd_unlock(controller);
}

bool x86_vtd_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                 const struct hal_iommu_map_request* request) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	vtd_lock(controller);
	struct iommu_pt_format format = vtd_format(controller);
	bool                   ok = iommu_pt_map(&format, space, request, vtd_sync, controller) && controller->initialized;
	vtd_unlock(controller);
	return ok;
}

bool x86_vtd_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   uint64_t io_address, size_t size) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	vtd_lock(controller);
	struct iommu_pt_format format = vtd_format(controller);
	bool ok = iommu_pt_unmap(&format, space, io_address, size, vtd_sync, controller) && controller->initialized;
	vtd_unlock(controller);
	return ok;
}

bool x86_vtd_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                    uint32_t source_id, struct hal_iommu_attachment_state* attachment) {
	if (!vtd_source_valid(controller, source_id) || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)controller || attachment == NULL || attachment->initialized)
		return false;
	vtd_lock(controller);
	uint64_t*         root       = vtd_root_virt(controller);
	unsigned          bus        = source_id >> 8u;
	unsigned          devfn      = source_id & 0xffu;
	uint64_t*         root_entry = &root[bus * 2u];
	uint64_t*         context_table;
	struct pmm_extent allocation = {0};
	if ((*root_entry & 1u) == 0u) {
		if (!iommu_pt_allocate_table(controller->table_allocation_size, &allocation)) goto fail;
		if (((uint64_t)allocation.address & ~controller->address_mask) != 0u) {
			(void)pmm_free(allocation);
			allocation = (struct pmm_extent){0};
			goto fail;
		}
		root_entry[1] = 0u;
		*root_entry   = allocation.address | 1u;
	}
	context_table         = (uint64_t*)iommu_pt_phys_to_virt((uintptr_t)(*root_entry & controller->address_mask));
	uint64_t* entry       = &context_table[devfn * 2u];
	unsigned  agaw        = controller->page_table_levels - 2u;
	uint64_t  expected_lo = space->table.root_address | 1u;
	uint64_t  expected_hi = ((uint64_t)space->table.context_id << 8u) | agaw;
	if ((entry[0] & 1u) != 0u) {
		vtd_unlock(controller);
		return false;
	}
	entry[1] = expected_hi;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	entry[0] = expected_lo;
	if (!vtd_context_invalidate(controller) || !vtd_iotlb_invalidate(controller)) hcf();
	*attachment = (struct hal_iommu_attachment_state){
		.initialized = true, .controller_identity = (uintptr_t)controller, .source_id = source_id};
	vtd_unlock(controller);
	return true;

fail:
	if (allocation.size != 0u) {
		*root_entry = 0u;
		if (!vtd_context_invalidate(controller)) hcf();
		(void)pmm_free(allocation);
	}
	vtd_unlock(controller);
	return false;
}

bool x86_vtd_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                    struct hal_iommu_attachment_state* attachment) {
	if (!vtd_source_valid(controller, source_id) || attachment == NULL || !attachment->initialized ||
	    attachment->controller_identity != (uintptr_t)controller || attachment->source_id != source_id)
		return false;
	vtd_lock(controller);
	uint64_t* root       = vtd_root_virt(controller);
	uint64_t* root_entry = &root[(source_id >> 8u) * 2u];
	if ((*root_entry & 1u) == 0u) {
		vtd_unlock(controller);
		return false;
	}
	uintptr_t context_address = (uintptr_t)(*root_entry & controller->address_mask);
	uint64_t* context_table   = (uint64_t*)iommu_pt_phys_to_virt(context_address);
	uint64_t* entry           = &context_table[(source_id & 0xffu) * 2u];
	if ((entry[0] & 1u) == 0u) {
		vtd_unlock(controller);
		return false;
	}
	entry[0] = 0u;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	entry[1] = 0u;
	if (!vtd_context_invalidate(controller) || !vtd_iotlb_invalidate(controller)) hcf();
	bool empty = true;
	for (size_t index = 0u; index < 256u; index++)
		if ((context_table[index * 2u] & 1u) != 0u) empty = false;
	if (empty) {
		*root_entry = 0u;
		if (!vtd_context_invalidate(controller)) hcf();
		(void)pmm_free((struct pmm_extent){.address = context_address, .size = controller->table_allocation_size});
	}
	*attachment = (struct hal_iommu_attachment_state){0};
	vtd_unlock(controller);
	return true;
}
