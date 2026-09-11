#include "amd.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <hal/hcf.h>
#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../iommu_page_table.h"

#define AMD_IOMMU_DEVICE_TABLE_SIZE ((size_t)65536u * 32u)
#define AMD_IOMMU_COMMAND_BUFFER_SIZE ((size_t)8192u)
#define AMD_IOMMU_V1_IO_ADDRESS_BITS 48u
#define AMD_IOMMU_COMMAND_ENTRY_SIZE 16u
#define AMD_IOMMU_REG_DEVICE_TABLE 0x0000u
#define AMD_IOMMU_REG_COMMAND_BUFFER 0x0008u
#define AMD_IOMMU_REG_CONTROL 0x0018u
#define AMD_IOMMU_REG_EXTENDED_FEATURES 0x0030u
#define AMD_IOMMU_REG_COMMAND_HEAD 0x2000u
#define AMD_IOMMU_REG_COMMAND_TAIL 0x2008u
#define AMD_IOMMU_REG_STATUS 0x2020u
#define AMD_IOMMU_REGISTER_SPACE_SIZE 0x4000u
#define AMD_IOMMU_CONTROL_ENABLE (1ull << 0u)
#define AMD_IOMMU_CONTROL_COMPLETION_WAIT (1ull << 4u)
#define AMD_IOMMU_CONTROL_COHERENT (1ull << 10u)
#define AMD_IOMMU_CONTROL_COMMAND_BUFFER (1ull << 12u)
#define AMD_IOMMU_STATUS_COMPLETION_WAIT (1ull << 2u)
#define AMD_IOMMU_COMMAND_BUFFER_512 (0x9ull << 56u)
#define AMD_IOMMU_COMMAND_INVALIDATE_DEVICE 2u
#define AMD_IOMMU_COMMAND_INVALIDATE_PAGES 3u
#define AMD_IOMMU_COMMAND_COMPLETION_WAIT 1u
#define AMD_IOMMU_WAIT_LIMIT 1000000u
#define AMD_IOMMU_DTE_VALID (1ull << 0u)
#define AMD_IOMMU_DTE_TRANSLATION_VALID (1ull << 1u)
#define AMD_IOMMU_DTE_READ (1ull << 61u)
#define AMD_IOMMU_DTE_WRITE (1ull << 62u)

struct amd_iommu_command {
	uint32_t data[4];
};

static void amd_lock(struct hal_iommu_controller_state* controller) {
	while (__atomic_exchange_n(&controller->operation_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
		__asm__ volatile("pause" : : : "memory");
}

static void amd_unlock(struct hal_iommu_controller_state* controller) {
	__atomic_store_n(&controller->operation_lock, 0u, __ATOMIC_RELEASE);
}

static inline volatile uint8_t* amd_registers(const struct hal_iommu_controller_state* controller) {
	return (volatile uint8_t*)controller->registers;
}

static inline uint64_t amd_read64(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint64_t*)(amd_registers(controller) + offset);
}

static inline uint32_t amd_read32(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint32_t*)(amd_registers(controller) + offset);
}

static inline void amd_write64(const struct hal_iommu_controller_state* controller, size_t offset, uint64_t value) {
	*(volatile uint64_t*)(amd_registers(controller) + offset) = value;
}

static inline void amd_write32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(amd_registers(controller) + offset) = value;
}

static bool amd_set_control(struct hal_iommu_controller_state* controller, uint64_t value) {
	const uint64_t mask = AMD_IOMMU_CONTROL_ENABLE | AMD_IOMMU_CONTROL_COMMAND_BUFFER;
	amd_write64(controller, AMD_IOMMU_REG_CONTROL, value);
	return (amd_read64(controller, AMD_IOMMU_REG_CONTROL) & mask) == (value & mask);
}

static unsigned amd_physical_address_bits(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u), "c"(0u));
	if (eax < 0x80000008u) return 0u;
	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000008u), "c"(0u));
	return eax & 0xffu;
}

static struct iommu_pt_format amd_format(const struct hal_iommu_controller_state* controller) {
	return (struct iommu_pt_format){.kind                  = IOMMU_PT_AMD_V1,
	                                .address_mask          = controller->address_mask,
	                                .leaf_size_mask        = controller->leaf_size_mask,
	                                .levels                = 4u,
	                                .io_address_bits       = controller->io_address_bits,
	                                .physical_address_bits = controller->physical_address_bits};
}

static bool amd_source_valid(const struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return controller != NULL && controller->initialized && controller->kind == HAL_IOMMU_KIND_AMD &&
	       source_id < 65536u;
}

static uint64_t* amd_device_entry(const struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return (uint64_t*)((uint8_t*)iommu_pt_phys_to_virt(controller->source_table_address) + source_id * 32u);
}

static bool amd_queue(struct hal_iommu_controller_state* controller, struct amd_iommu_command command) {
	for (size_t attempt = 0u; attempt < AMD_IOMMU_WAIT_LIMIT; attempt++) {
		uint32_t next =
			(controller->command_queue_tail + AMD_IOMMU_COMMAND_ENTRY_SIZE) & (AMD_IOMMU_COMMAND_BUFFER_SIZE - 1u);
		if (next !=
		    (uint32_t)(amd_read32(controller, AMD_IOMMU_REG_COMMAND_HEAD) & (AMD_IOMMU_COMMAND_BUFFER_SIZE - 1u))) {
			struct amd_iommu_command* entries = iommu_pt_phys_to_virt(controller->command_queue_address);
			entries[controller->command_queue_tail / AMD_IOMMU_COMMAND_ENTRY_SIZE] = command;
			__atomic_thread_fence(__ATOMIC_RELEASE);
			controller->command_queue_tail = next;
			amd_write32(controller, AMD_IOMMU_REG_COMMAND_TAIL, next);
			return true;
		}
		__asm__ volatile("pause" : : : "memory");
	}
	return false;
}

static bool amd_complete(struct hal_iommu_controller_state* controller) {
	amd_write32(controller, AMD_IOMMU_REG_STATUS, AMD_IOMMU_STATUS_COMPLETION_WAIT);
	struct amd_iommu_command command = {
		.data = {2u, AMD_IOMMU_COMMAND_COMPLETION_WAIT << 28u, 0u, 0u}
    };
	if (!amd_queue(controller, command)) return false;
	for (size_t attempt = 0u; attempt < AMD_IOMMU_WAIT_LIMIT; attempt++) {
		if ((amd_read32(controller, AMD_IOMMU_REG_STATUS) & AMD_IOMMU_STATUS_COMPLETION_WAIT) != 0u) return true;
		__asm__ volatile("pause" : : : "memory");
	}
	return false;
}

static bool amd_invalidate_device(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	struct amd_iommu_command command = {
		.data = {source_id, AMD_IOMMU_COMMAND_INVALIDATE_DEVICE << 28u, 0u, 0u}
    };
	return amd_queue(controller, command) && amd_complete(controller);
}

static bool amd_invalidate_domain(struct hal_iommu_controller_state* controller, uint32_t context_id,
                                  bool hierarchy_changed) {
	const uint64_t           address = 0x7ffffffffffff000ull;
	struct amd_iommu_command command = {
		.data = {0u,
	             context_id | (AMD_IOMMU_COMMAND_INVALIDATE_PAGES << 28u),
	             (uint32_t)address | 1u | (hierarchy_changed ? 2u : 0u),
	             (uint32_t)(address >> 32u)}
    };
	return amd_queue(controller, command) && amd_complete(controller);
}

static bool amd_sync(void* context, uint32_t context_id, uint64_t io_address, size_t size, bool hierarchy_changed) {
	(void)io_address;
	(void)size;
	return amd_invalidate_domain(context, context_id, hierarchy_changed);
}

bool x86_amd_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                                   const struct hal_iommu_controller_descriptor* descriptor,
                                   struct hal_iommu_info*                        out_info) {
	const struct pmm_info* pmm = pmm_info();
	struct pmm_extent      devices;
	struct pmm_extent      commands;
	unsigned               physical_bits;
	if (controller == NULL || descriptor == NULL || out_info == NULL || descriptor->kind != HAL_IOMMU_KIND_AMD ||
	    descriptor->register_address == 0u || pmm == NULL)
		return false;
	if (!iommu_pt_map_mmio(descriptor->register_address, AMD_IOMMU_REGISTER_SPACE_SIZE)) return false;
	physical_bits = amd_physical_address_bits();
	if (descriptor->firmware_physical_address_bits != 0u && descriptor->firmware_physical_address_bits < physical_bits)
		physical_bits = descriptor->firmware_physical_address_bits;
	unsigned io_bits = descriptor->firmware_io_address_bits;
	if (io_bits == 0u) io_bits = 64u;
	if (physical_bits < 32u || physical_bits > 52u || io_bits < 32u || io_bits > 64u ||
	    pmm->allocation_granule > AMD_IOMMU_COMMAND_BUFFER_SIZE)
		return false;
	if (io_bits > AMD_IOMMU_V1_IO_ADDRESS_BITS) io_bits = AMD_IOMMU_V1_IO_ADDRESS_BITS;
	if (!iommu_pt_allocate_table(AMD_IOMMU_DEVICE_TABLE_SIZE, &devices)) return false;
	if (!iommu_pt_allocate_table(AMD_IOMMU_COMMAND_BUFFER_SIZE, &commands)) {
		(void)pmm_free(devices);
		return false;
	}
	uint64_t address_mask = ((1ull << physical_bits) - 1u) & ~(uint64_t)(IOMMU_PT_PAGE_SIZE - 1u);
	if (((uint64_t)devices.address & ~address_mask) != 0u || ((uint64_t)commands.address & ~address_mask) != 0u) {
		(void)pmm_free(commands);
		(void)pmm_free(devices);
		return false;
	}
	*controller = (struct hal_iommu_controller_state){
		.kind                  = HAL_IOMMU_KIND_AMD,
		.registers             = iommu_pt_phys_to_virt(descriptor->register_address),
		.source_table_address  = devices.address,
		.command_queue_address = commands.address,
		.table_allocation_size =
			pmm->allocation_granule > IOMMU_PT_PAGE_SIZE ? pmm->allocation_granule : IOMMU_PT_PAGE_SIZE,
		.source_table_size     = devices.size,
		.command_queue_size    = commands.size,
		.physical_address_bits = (uint8_t)physical_bits,
		.io_address_bits       = (uint8_t)io_bits,
		.context_id_bits       = 16u,
		.source_id_bits        = 16u,
		.context_limit         = UINT16_MAX,
		.address_mask          = address_mask,
		.leaf_size_mask        = (1ull << IOMMU_PT_PAGE_SHIFT) | (1ull << 21u) | (1ull << 30u),
		.extended_capabilities = 0u};
	controller->extended_capabilities = amd_read64(controller, AMD_IOMMU_REG_EXTENDED_FEATURES);
	controller->control               = amd_read64(controller, AMD_IOMMU_REG_CONTROL);
	controller->control &= ~(AMD_IOMMU_CONTROL_ENABLE | AMD_IOMMU_CONTROL_COMMAND_BUFFER | AMD_IOMMU_CONTROL_COHERENT);
	if ((descriptor->firmware_flags & (1u << 5u)) != 0u) controller->control |= AMD_IOMMU_CONTROL_COHERENT;
	if (!amd_set_control(controller, controller->control)) {
		(void)pmm_free(commands);
		(void)pmm_free(devices);
		return false;
	}
	amd_write64(controller,
	            AMD_IOMMU_REG_DEVICE_TABLE,
	            devices.address | (uint64_t)((devices.size >> IOMMU_PT_PAGE_SHIFT) - 1u));
	amd_write64(controller, AMD_IOMMU_REG_COMMAND_BUFFER, commands.address | AMD_IOMMU_COMMAND_BUFFER_512);
	amd_write32(controller, AMD_IOMMU_REG_COMMAND_HEAD, 0u);
	amd_write32(controller, AMD_IOMMU_REG_COMMAND_TAIL, 0u);
	controller->control |=
		AMD_IOMMU_CONTROL_ENABLE | AMD_IOMMU_CONTROL_COMMAND_BUFFER | AMD_IOMMU_CONTROL_COMPLETION_WAIT;
	if (!amd_set_control(controller, controller->control)) hcf();
	controller->initialized = true;
	*out_info               = (struct hal_iommu_info){.kind                  = HAL_IOMMU_KIND_AMD,
	                                                  .minimum_leaf_size     = IOMMU_PT_PAGE_SIZE,
	                                                  .leaf_size_mask        = controller->leaf_size_mask,
	                                                  .io_address_bits       = controller->io_address_bits,
	                                                  .physical_address_bits = controller->physical_address_bits,
	                                                  .context_id_bits       = controller->context_id_bits,
	                                                  .source_id_bits        = controller->source_id_bits};
	return true;
}

void x86_amd_iommu_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (!amd_source_valid(controller, 0u)) return;
	amd_lock(controller);
	uint64_t* devices = iommu_pt_phys_to_virt(controller->source_table_address);
	for (size_t index = 0u; index < AMD_IOMMU_DEVICE_TABLE_SIZE / sizeof(uint64_t); index += 4u)
		if ((devices[index] & AMD_IOMMU_DTE_VALID) != 0u) {
			amd_unlock(controller);
			return;
		}
	controller->control &= ~(AMD_IOMMU_CONTROL_ENABLE | AMD_IOMMU_CONTROL_COMMAND_BUFFER);
	if (!amd_set_control(controller, controller->control)) hcf();
	controller->initialized = false;
	(void)pmm_free((struct pmm_extent){controller->command_queue_address, controller->command_queue_size});
	(void)pmm_free((struct pmm_extent){controller->source_table_address, controller->source_table_size});
	amd_unlock(controller);
}

bool x86_amd_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	if (controller == NULL || !controller->initialized || controller->kind != HAL_IOMMU_KIND_AMD) return false;
	struct iommu_pt_format format = amd_format(controller);
	return iommu_pt_access_supported(&format, access);
}

bool x86_amd_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                              struct hal_iommu_space_state* space) {
	if (controller == NULL || !controller->initialized || space == NULL || context_id > UINT16_MAX) return false;
	amd_lock(controller);
	struct iommu_pt_format format = amd_format(controller);
	bool                   ok     = iommu_pt_space_init(&format, (uintptr_t)controller, context_id, space);
	amd_unlock(controller);
	return ok;
}

void x86_amd_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	if (controller == NULL || space == NULL || space->table.controller_identity != (uintptr_t)controller) return;
	amd_lock(controller);
	struct iommu_pt_format format = amd_format(controller);
	iommu_pt_space_deinit(&format, space);
	amd_unlock(controller);
}

bool x86_amd_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                       const struct hal_iommu_map_request* request) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	amd_lock(controller);
	struct iommu_pt_format format = amd_format(controller);
	bool                   ok = iommu_pt_map(&format, space, request, amd_sync, controller) && controller->initialized;
	amd_unlock(controller);
	return ok;
}

bool x86_amd_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                         uint64_t io_address, size_t size) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	amd_lock(controller);
	struct iommu_pt_format format = amd_format(controller);
	bool ok = iommu_pt_unmap(&format, space, io_address, size, amd_sync, controller) && controller->initialized;
	amd_unlock(controller);
	return ok;
}

bool x86_amd_iommu_protect(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                           uint64_t io_address, size_t size, uint64_t access) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	amd_lock(controller);
	struct iommu_pt_format format = amd_format(controller);
	bool                   ok =
		iommu_pt_protect(&format, space, io_address, size, access, amd_sync, controller) && controller->initialized;
	amd_unlock(controller);
	return ok;
}

bool x86_amd_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                          uint32_t source_id, struct hal_iommu_attachment_state* attachment) {
	if (!amd_source_valid(controller, source_id) || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)controller || attachment == NULL || attachment->initialized)
		return false;
	amd_lock(controller);
	uint64_t* entry    = amd_device_entry(controller, source_id);
	uint64_t  expected = space->table.root_address | AMD_IOMMU_DTE_VALID | AMD_IOMMU_DTE_TRANSLATION_VALID |
	                    (4ull << 9u) | AMD_IOMMU_DTE_READ | AMD_IOMMU_DTE_WRITE;
	if ((entry[0] & AMD_IOMMU_DTE_VALID) != 0u) {
		amd_unlock(controller);
		return false;
	}
	entry[1] = space->table.context_id;
	entry[2] = 0u;
	entry[3] = 0u;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	entry[0] = expected;
	if (!amd_invalidate_device(controller, source_id)) hcf();
	*attachment = (struct hal_iommu_attachment_state){
		.initialized = true, .controller_identity = (uintptr_t)controller, .source_id = source_id};
	amd_unlock(controller);
	return true;
}

bool x86_amd_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                          struct hal_iommu_attachment_state* attachment) {
	if (!amd_source_valid(controller, source_id) || attachment == NULL || !attachment->initialized ||
	    attachment->controller_identity != (uintptr_t)controller || attachment->source_id != source_id)
		return false;
	amd_lock(controller);
	uint64_t* entry = amd_device_entry(controller, source_id);
	if ((entry[0] & AMD_IOMMU_DTE_VALID) == 0u) {
		amd_unlock(controller);
		return false;
	}
	entry[0] = 0u;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	entry[1] = entry[2] = entry[3] = 0u;
	if (!amd_invalidate_device(controller, source_id)) hcf();
	*attachment = (struct hal_iommu_attachment_state){0};
	amd_unlock(controller);
	return true;
}
