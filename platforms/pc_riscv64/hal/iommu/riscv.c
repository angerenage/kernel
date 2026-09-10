#include "riscv.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_page_table.h"

#define RI_CAP 0x000u
#define RI_FCTL 0x008u
#define RI_DDTP 0x010u
#define RI_CQB 0x018u
#define RI_CQH 0x020u
#define RI_CQT 0x024u
#define RI_CQCSR 0x048u
#define RI_CQEN (1u << 0u)
#define RI_CQON (1u << 16u)
#define RI_CQBUSY (1u << 17u)
#define RI_DDTP_BUSY (1ull << 4u)
#define RI_DDTP_1LVL 2u
#define RI_DDTP_2LVL 3u
#define RI_DDTP_3LVL 4u
#define RI_QUEUE_LOG2 8u
#define RI_WAIT_LIMIT 1000000u
#define RI_CMD_IOTINVAL 1u
#define RI_CMD_IOFENCE 2u
#define RI_CMD_IODIR 3u

struct ri_command {
	uint64_t word[2];
};
struct ri_ddt_path {
	uint64_t* table[3];
	uint16_t  index[3];
	uint8_t   depth;
};

static void ri_lock(struct hal_iommu_controller_state* c) {
	while (__atomic_exchange_n(&c->operation_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop" : : : "memory");
}

static void ri_unlock(struct hal_iommu_controller_state* c) {
	__atomic_store_n(&c->operation_lock, 0u, __ATOMIC_RELEASE);
}

static inline volatile uint8_t* ri_regs(const struct hal_iommu_controller_state* c) {
	return (volatile uint8_t*)c->registers;
}

static inline uint32_t ri_read32(const struct hal_iommu_controller_state* c, size_t offset) {
	return *(volatile uint32_t*)(ri_regs(c) + offset);
}

static inline uint64_t ri_read64(const struct hal_iommu_controller_state* c, size_t offset) {
	return *(volatile uint64_t*)(ri_regs(c) + offset);
}

static inline void ri_write32(const struct hal_iommu_controller_state* c, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(ri_regs(c) + offset) = value;
}

static inline void ri_write64(const struct hal_iommu_controller_state* c, size_t offset, uint64_t value) {
	*(volatile uint64_t*)(ri_regs(c) + offset) = value;
}

static bool ri_wait32(const struct hal_iommu_controller_state* c, size_t offset, uint32_t mask, uint32_t value) {
	for (size_t n = 0u; n < RI_WAIT_LIMIT; n++) {
		if ((ri_read32(c, offset) & mask) == value) return true;
		__asm__ volatile("nop" : : : "memory");
	}
	return false;
}

static bool ri_wait_ddtp(const struct hal_iommu_controller_state* c) {
	for (size_t n = 0u; n < RI_WAIT_LIMIT; n++) {
		if ((ri_read64(c, RI_DDTP) & RI_DDTP_BUSY) == 0u) return true;
		__asm__ volatile("nop" : : : "memory");
	}
	return false;
}

static struct iommu_pt_format ri_format(const struct hal_iommu_controller_state* c) {
	uint8_t levels = c->io_address_bits == 57u ? 5u : c->io_address_bits == 48u ? 4u : 3u;
	return (struct iommu_pt_format){
		IOMMU_PT_RISCV, c->address_mask, c->leaf_size_mask, levels, c->io_address_bits, c->physical_address_bits};
}

static bool ri_queue(struct hal_iommu_controller_state* c, struct ri_command command) {
	uint32_t entries = 1u << c->command_queue_log2_entries;
	for (size_t n = 0u; n < RI_WAIT_LIMIT; n++) {
		uint32_t next = (c->command_queue_tail + 1u) & (entries - 1u);
		if (next != (ri_read32(c, RI_CQH) & (entries - 1u))) {
			struct ri_command* queue     = iommu_pt_phys_to_virt(c->command_queue_address);
			queue[c->command_queue_tail] = command;
			__asm__ volatile("fence w,o" : : : "memory");
			c->command_queue_tail = next;
			ri_write32(c, RI_CQT, next);
			return true;
		}
		__asm__ volatile("nop" : : : "memory");
	}
	return false;
}

static bool ri_complete(struct hal_iommu_controller_state* c) {
	if (!ri_queue(c,
	              (struct ri_command){
					  .word = {RI_CMD_IOFENCE, 0u}
    }))
		return false;
	for (size_t n = 0u; n < RI_WAIT_LIMIT; n++) {
		if (ri_read32(c, RI_CQH) == c->command_queue_tail) return true;
		__asm__ volatile("nop" : : : "memory");
	}
	return false;
}

static bool ri_invalidate_device(struct hal_iommu_controller_state* c, uint32_t source_id) {
	uint64_t command = RI_CMD_IODIR | (1ull << 33u) | ((uint64_t)source_id << 40u);
	return ri_queue(c,
	                (struct ri_command){
						.word = {command, 0u}
    }) &&
	       ri_complete(c);
}

static bool ri_sync(void* context, uint32_t context_id, uint64_t io_address, size_t size, bool hierarchy_changed) {
	(void)io_address;
	(void)size;
	struct hal_iommu_controller_state* c       = context;
	uint64_t                           command = RI_CMD_IOTINVAL | ((uint64_t)context_id << 12u) | (1ull << 32u);
	if (hierarchy_changed && (c->capabilities & (1ull << 42u)) != 0u) command |= 1ull << 34u;
	return ri_queue(c,
	                (struct ri_command){
						.word = {command, 0u}
    }) &&
	       ri_complete(c);
}

static bool ri_table_empty(const uint64_t* table) {
	for (size_t i = 0u; i < 512u; i++)
		if ((table[i] & 1u) != 0u) return false;
	return true;
}

static void ri_indices(const struct hal_iommu_controller_state* c, uint32_t source_id, uint16_t index[3]) {
	if (c->device_context_size == 64u) {
		index[0] = source_id & 0x3fu;
		index[1] = (source_id >> 6u) & 0x1ffu;
		index[2] = (source_id >> 15u) & 0x1ffu;
	}
	else {
		index[0] = source_id & 0x7fu;
		index[1] = (source_id >> 7u) & 0x1ffu;
		index[2] = (source_id >> 16u) & 0xffu;
	}
}

static uint64_t* ri_device_context(struct hal_iommu_controller_state* c, uint32_t source_id, bool allocate,
                                   struct ri_ddt_path* out_path) {
	uint16_t index[3];
	ri_indices(c, source_id, index);
	uint64_t*          table = iommu_pt_phys_to_virt(c->device_directory_address);
	struct ri_ddt_path path  = {.depth = c->device_directory_levels};
	uint64_t*          allocated_parent[2];
	uint16_t           allocated_index[2];
	struct pmm_extent  allocated_extent[2];
	uint8_t            allocated_count = 0u;
	for (uint8_t depth = c->device_directory_levels; depth > 1u; depth--) {
		uint8_t slot     = depth - 1u;
		path.table[slot] = table;
		path.index[slot] = index[slot];
		uint64_t entry   = table[index[slot]];
		if ((entry & 1u) == 0u) {
			if (!allocate) return NULL;
			struct pmm_extent child = {0};
			if (!iommu_pt_allocate_table(c->table_allocation_size, &child) ||
			    ((uint64_t)child.address & ~c->address_mask) != 0u) {
				if (child.size != 0u) (void)pmm_free(child);
				goto fail;
			}
			allocated_parent[allocated_count] = table;
			allocated_index[allocated_count]  = index[slot];
			allocated_extent[allocated_count] = child;
			allocated_count++;
			entry              = ((uint64_t)child.address >> 2u) | 1u;
			table[index[slot]] = entry;
			__asm__ volatile("fence w,w" : : : "memory");
		}
		table = iommu_pt_phys_to_virt((uintptr_t)((entry >> 10u) << 12u));
	}
	path.table[0] = table;
	path.index[0] = index[0];
	if (out_path != NULL) *out_path = path;
	return (uint64_t*)((uint8_t*)table + (size_t)index[0] * c->device_context_size);

fail:
	while (allocated_count != 0u) {
		allocated_count--;
		allocated_parent[allocated_count][allocated_index[allocated_count]] = 0u;
		__asm__ volatile("fence w,w" : : : "memory");
		(void)pmm_free(allocated_extent[allocated_count]);
	}
	return NULL;
}

static void ri_reclaim_ddt(struct hal_iommu_controller_state* c, const struct ri_ddt_path* path) {
	for (uint8_t depth = 1u; depth < path->depth; depth++) {
		uint64_t* child = path->table[depth - 1u];
		if (!ri_table_empty(child)) break;
		uint64_t* parent           = path->table[depth];
		uintptr_t address          = (uintptr_t)((parent[path->index[depth]] >> 10u) << 12u);
		parent[path->index[depth]] = 0u;
		(void)pmm_free((struct pmm_extent){address, c->table_allocation_size});
	}
}

static bool ri_source_valid(const struct hal_iommu_controller_state* c, uint32_t source_id) {
	return c != NULL && c->initialized && source_id < (1u << c->source_id_bits);
}

bool riscv_iommu_controller_init(struct hal_iommu_controller_state*            c,
                                 const struct hal_iommu_controller_descriptor* descriptor,
                                 struct hal_iommu_info*                        out_info) {
	const struct pmm_info* pmm       = pmm_info();
	struct pmm_extent      directory = {0}, commands = {0};
	if (c == NULL || descriptor == NULL || out_info == NULL || pmm == NULL ||
	    descriptor->kind != HAL_IOMMU_KIND_RISCV || descriptor->register_address == 0u)
		return false;
	*c = (struct hal_iommu_controller_state){.registers = iommu_pt_phys_to_virt(descriptor->register_address)};
	uint64_t cap     = ri_read64(c, RI_CAP);
	uint8_t  version = cap & 0xffu, physical_bits = (cap >> 32u) & 0x3fu, io_bits, pt_levels;
	if (version == 0u || physical_bits < 32u || physical_bits > 56u) return false;
	if ((cap & (1ull << 11u)) != 0u) {
		io_bits   = 57u;
		pt_levels = 5u;
	}
	else if ((cap & (1ull << 10u)) != 0u) {
		io_bits   = 48u;
		pt_levels = 4u;
	}
	else if ((cap & (1ull << 9u)) != 0u) {
		io_bits   = 39u;
		pt_levels = 3u;
	}
	else return false;
	size_t allocation_size = pmm->allocation_granule > 4096u ? pmm->allocation_granule : 4096u;
	if (!iommu_pt_allocate_table(allocation_size, &directory) ||
	    !iommu_pt_allocate_table(allocation_size > ((size_t)16u << RI_QUEUE_LOG2) ? allocation_size
	                                                                              : ((size_t)16u << RI_QUEUE_LOG2),
	                             &commands))
		goto fail;
	uint64_t address_mask = ((1ull << physical_bits) - 1u) & ~0xfffull;
	if (((directory.address | commands.address) & ~address_mask) != 0u) goto fail;
	c->device_directory_address   = directory.address;
	c->device_directory_size      = directory.size;
	c->command_queue_address      = commands.address;
	c->command_queue_size         = commands.size;
	c->table_allocation_size      = allocation_size;
	c->capabilities               = cap;
	c->address_mask               = address_mask;
	c->io_address_bits            = io_bits;
	c->physical_address_bits      = physical_bits;
	c->context_id_bits            = 20u;
	c->device_context_size        = (cap & (1ull << 22u)) != 0u ? 64u : 32u;
	c->command_queue_log2_entries = RI_QUEUE_LOG2;
	for (uint8_t level = 0u; level < pt_levels; level++) c->leaf_size_mask |= 1ull << (12u + 9u * level);
	ri_write32(c, RI_CQCSR, 0u);
	if (!ri_wait32(c, RI_CQCSR, RI_CQON | RI_CQBUSY, 0u)) goto fail;
	ri_write32(c, RI_FCTL, 0u);
	ri_write64(c, RI_CQB, ((uint64_t)commands.address >> 2u) | (RI_QUEUE_LOG2 - 1u));
	ri_write32(c, RI_CQT, 0u);
	ri_write32(c, RI_CQCSR, RI_CQEN);
	if (!ri_wait32(c, RI_CQCSR, RI_CQON, RI_CQON)) goto fail;
	ri_write64(c, RI_DDTP, 0u);
	if (!ri_wait_ddtp(c)) goto fail;
	for (uint8_t mode = RI_DDTP_3LVL; mode >= RI_DDTP_1LVL; mode--) {
		ri_write64(c, RI_DDTP, ((uint64_t)directory.address >> 2u) | mode);
		if (ri_wait_ddtp(c) && (ri_read64(c, RI_DDTP) & 0xfu) == mode) {
			c->device_directory_levels = mode - 1u;
			break;
		}
	}
	if (c->device_directory_levels == 0u) goto fail;
	c->source_id_bits = c->device_context_size == 64u ? (uint8_t)(6u + 9u * (c->device_directory_levels - 1u))
	                                                  : (uint8_t)(7u + 9u * (c->device_directory_levels - 1u));
	if (c->source_id_bits > 24u) c->source_id_bits = 24u;
	c->initialized = true;
	*out_info      = (struct hal_iommu_info){
        HAL_IOMMU_KIND_RISCV, 4096u, c->leaf_size_mask, io_bits, physical_bits, 20u, c->source_id_bits};
	return true;
fail:
	ri_write64(c, RI_DDTP, 0u);
	ri_write32(c, RI_CQCSR, 0u);
	if (commands.size != 0u) (void)pmm_free(commands);
	if (directory.size != 0u) (void)pmm_free(directory);
	*c = (struct hal_iommu_controller_state){0};
	return false;
}

void riscv_iommu_controller_deinit(struct hal_iommu_controller_state* c) {
	if (c == NULL || !c->initialized || c->device_count != 0u) return;
	ri_lock(c);
	ri_write64(c, RI_DDTP, 0u);
	(void)ri_wait_ddtp(c);
	ri_write32(c, RI_CQCSR, 0u);
	(void)ri_wait32(c, RI_CQCSR, RI_CQON | RI_CQBUSY, 0u);
	c->initialized = false;
	(void)pmm_free((struct pmm_extent){c->command_queue_address, c->command_queue_size});
	(void)pmm_free((struct pmm_extent){c->device_directory_address, c->device_directory_size});
	ri_unlock(c);
}

bool riscv_iommu_mapping_supported(const struct hal_iommu_controller_state* c, uint64_t access) {
	if (c == NULL || !c->initialized) return false;
	struct iommu_pt_format f = ri_format(c);
	return iommu_pt_access_supported(&f, access);
}

bool riscv_iommu_space_init(struct hal_iommu_controller_state* c, uint32_t context_id,
                            struct hal_iommu_space_state* space) {
	if (c == NULL || !c->initialized || space == NULL || context_id >= (1u << 20u)) return false;
	ri_lock(c);
	struct iommu_pt_format f  = ri_format(c);
	bool                   ok = iommu_pt_space_init(&f, (uintptr_t)c, context_id, space);
	ri_unlock(c);
	return ok;
}

void riscv_iommu_space_deinit(struct hal_iommu_controller_state* c, struct hal_iommu_space_state* space) {
	if (c == NULL || space == NULL || space->table.controller_identity != (uintptr_t)c) return;
	ri_lock(c);
	struct iommu_pt_format f = ri_format(c);
	iommu_pt_space_deinit(&f, space);
	ri_unlock(c);
}

bool riscv_iommu_map(struct hal_iommu_controller_state* c, struct hal_iommu_space_state* space,
                     const struct hal_iommu_map_request* request) {
	if (c == NULL || !c->initialized || space == NULL || space->table.controller_identity != (uintptr_t)c) return false;
	ri_lock(c);
	struct iommu_pt_format f  = ri_format(c);
	bool                   ok = iommu_pt_map(&f, space, request, ri_sync, c);
	ri_unlock(c);
	return ok;
}

bool riscv_iommu_unmap(struct hal_iommu_controller_state* c, struct hal_iommu_space_state* space, uint64_t io_address,
                       size_t size) {
	if (c == NULL || !c->initialized || space == NULL || space->table.controller_identity != (uintptr_t)c) return false;
	ri_lock(c);
	struct iommu_pt_format f  = ri_format(c);
	bool                   ok = iommu_pt_unmap(&f, space, io_address, size, ri_sync, c);
	ri_unlock(c);
	return ok;
}

bool riscv_iommu_attach(struct hal_iommu_controller_state* c, struct hal_iommu_space_state* space, uint32_t source_id) {
	if (!ri_source_valid(c, source_id) || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)c)
		return false;
	ri_lock(c);
	struct ri_ddt_path path;
	uint64_t*          dc = ri_device_context(c, source_id, true, &path);
	if (dc == NULL) {
		ri_unlock(c);
		return false;
	}
	uint8_t  mode   = c->io_address_bits == 57u ? 10u : c->io_address_bits == 48u ? 9u : 8u;
	uint64_t iosatp = ((uint64_t)mode << 60u) | ((uint64_t)space->table.root_address >> 12u);
	if ((dc[0] & 1u) != 0u) {
		bool same = dc[2] == ((uint64_t)space->table.context_id << 12u) && dc[3] == iosatp;
		ri_unlock(c);
		return same;
	}
	dc[1] = 0u;
	dc[2] = (uint64_t)space->table.context_id << 12u;
	dc[3] = iosatp;
	if (c->device_context_size == 64u) dc[4] = dc[5] = dc[6] = dc[7] = 0u;
	__asm__ volatile("fence w,w" : : : "memory");
	dc[0] = 1u;
	__asm__ volatile("fence w,o" : : : "memory");
	if (!ri_invalidate_device(c, source_id)) {
		dc[0] = 0u;
		memset(dc, 0, c->device_context_size);
		ri_reclaim_ddt(c, &path);
		ri_unlock(c);
		return false;
	}
	c->device_count++;
	ri_unlock(c);
	return true;
}

bool riscv_iommu_detach(struct hal_iommu_controller_state* c, uint32_t source_id) {
	if (!ri_source_valid(c, source_id)) return false;
	ri_lock(c);
	struct ri_ddt_path path;
	uint64_t*          dc = ri_device_context(c, source_id, false, &path);
	if (dc == NULL || (dc[0] & 1u) == 0u) {
		ri_unlock(c);
		return false;
	}
	dc[0] = 0u;
	__asm__ volatile("fence w,o" : : : "memory");
	bool ok = ri_invalidate_device(c, source_id);
	if (ok) {
		memset(dc, 0, c->device_context_size);
		ri_reclaim_ddt(c, &path);
		c->device_count--;
	}
	ri_unlock(c);
	return ok;
}
