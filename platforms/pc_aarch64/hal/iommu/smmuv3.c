#include "smmuv3.h"

#include <core/mm.h>
#include <core/pmm.h>
#include <hal/iommu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_page_table.h"

#define SMMU_IDR0 0x000u
#define SMMU_IDR1 0x004u
#define SMMU_IDR5 0x014u
#define SMMU_CR0 0x020u
#define SMMU_CR0ACK 0x024u
#define SMMU_CR1 0x028u
#define SMMU_CR2 0x02cu
#define SMMU_GBPA 0x044u
#define SMMU_STRTAB_BASE 0x080u
#define SMMU_STRTAB_BASE_CFG 0x088u
#define SMMU_CMDQ_BASE 0x090u
#define SMMU_CMDQ_PROD 0x098u
#define SMMU_CMDQ_CONS 0x09cu
#define SMMU_EVTQ_BASE 0x0a0u
#define SMMU_EVTQ_PROD 0x0a8u
#define SMMU_EVTQ_CONS 0x0acu
#define SMMU_CR0_SMMUEN (1u << 0u)
#define SMMU_CR0_EVTQEN (1u << 2u)
#define SMMU_CR0_CMDQEN (1u << 3u)
#define SMMU_GBPA_UPDATE (1u << 31u)
#define SMMU_GBPA_ABORT (1u << 20u)
#define SMMU_BASE_RA (1ull << 62u)
#define SMMU_STE_VALID (1ull << 0u)
#define SMMU_STE_CFG_S2 (6ull << 1u)
#define SMMU_CMD_CFGI_STE 0x03u
#define SMMU_CMD_CFGI_ALL 0x04u
#define SMMU_CMD_TLBI_S12_VMALL 0x28u
#define SMMU_CMD_SYNC 0x46u
#define SMMU_QUEUE_LOG2_ENTRIES 8u
#define SMMU_COMMAND_SIZE 16u
#define SMMU_EVENT_SIZE 32u
#define SMMU_STE_SIZE 64u
#define SMMU_L2_STREAM_BITS 8u
#define SMMU_WAIT_LIMIT 1000000u

struct smmu_command {
	uint64_t word[2];
};

static void smmu_lock(struct hal_iommu_controller_state* controller) {
	while (__atomic_exchange_n(&controller->operation_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
		__asm__ volatile("yield" : : : "memory");
}

static void smmu_unlock(struct hal_iommu_controller_state* controller) {
	__atomic_store_n(&controller->operation_lock, 0u, __ATOMIC_RELEASE);
}

static inline volatile uint8_t* smmu_registers(const struct hal_iommu_controller_state* controller) {
	return (volatile uint8_t*)controller->registers;
}

static inline uint32_t smmu_read32(const struct hal_iommu_controller_state* controller, size_t offset) {
	return *(volatile uint32_t*)(smmu_registers(controller) + offset);
}

static inline void smmu_write32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t value) {
	*(volatile uint32_t*)(smmu_registers(controller) + offset) = value;
}

static inline void smmu_write64(const struct hal_iommu_controller_state* controller, size_t offset, uint64_t value) {
	*(volatile uint64_t*)(smmu_registers(controller) + offset) = value;
}

static bool smmu_wait32(const struct hal_iommu_controller_state* controller, size_t offset, uint32_t value) {
	for (size_t attempt = 0u; attempt < SMMU_WAIT_LIMIT; attempt++) {
		if (smmu_read32(controller, offset) == value) return true;
		__asm__ volatile("yield" : : : "memory");
	}
	return false;
}

static bool smmu_update_cr0(const struct hal_iommu_controller_state* controller, uint32_t value) {
	smmu_write32(controller, SMMU_CR0, value);
	return smmu_wait32(controller, SMMU_CR0ACK, value);
}

static struct iommu_pt_format smmu_format(const struct hal_iommu_controller_state* controller) {
	return (struct iommu_pt_format){.kind                  = IOMMU_PT_ARM_STAGE2,
	                                .address_mask          = controller->address_mask,
	                                .leaf_size_mask        = controller->leaf_size_mask,
	                                .levels                = 4u,
	                                .io_address_bits       = controller->io_address_bits,
	                                .physical_address_bits = controller->physical_address_bits};
}

static bool smmu_queue(struct hal_iommu_controller_state* controller, struct smmu_command command) {
	uint32_t entries = 1u << controller->command_queue_log2_entries;
	uint32_t mask    = (entries << 1u) - 1u;
	for (size_t attempt = 0u; attempt < SMMU_WAIT_LIMIT; attempt++) {
		uint32_t next = (controller->command_queue_producer + 1u) & mask;
		if (next != (smmu_read32(controller, SMMU_CMDQ_CONS) & mask)) {
			struct smmu_command* queue = iommu_pt_phys_to_virt(controller->command_queue_address);
			queue[controller->command_queue_producer & (entries - 1u)] = command;
			__asm__ volatile("dsb oshst" : : : "memory");
			controller->command_queue_producer = next;
			smmu_write32(controller, SMMU_CMDQ_PROD, next);
			return true;
		}
		__asm__ volatile("yield" : : : "memory");
	}
	return false;
}

static bool smmu_complete(struct hal_iommu_controller_state* controller) {
	if (!smmu_queue(controller,
	                (struct smmu_command){
						.word = {SMMU_CMD_SYNC, 0u}
    }))
		return false;
	uint32_t mask = (2u << controller->command_queue_log2_entries) - 1u;
	for (size_t attempt = 0u; attempt < SMMU_WAIT_LIMIT; attempt++) {
		if ((smmu_read32(controller, SMMU_CMDQ_CONS) & mask) == controller->command_queue_producer) return true;
		__asm__ volatile("yield" : : : "memory");
	}
	return false;
}

static bool smmu_invalidate_stream(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return smmu_queue(controller,
	                  (struct smmu_command){
						  .word = {SMMU_CMD_CFGI_STE | ((uint64_t)source_id << 32u), 1u}
    }) &&
	       smmu_complete(controller);
}

static bool smmu_sync(void* context, uint32_t context_id, uint64_t io_address, size_t size, bool hierarchy_changed) {
	(void)io_address;
	(void)size;
	(void)hierarchy_changed;
	struct hal_iommu_controller_state* controller = context;
	return smmu_queue(controller,
	                  (struct smmu_command){
						  .word = {SMMU_CMD_TLBI_S12_VMALL | ((uint64_t)context_id << 32u), 0u}
    }) &&
	       smmu_complete(controller);
}

static uint8_t smmu_oas_bits(uint32_t idr5) {
	static const uint8_t bits[] = {32u, 36u, 40u, 42u, 44u, 48u, 52u, 0u};
	return bits[idr5 & 7u];
}

static uint8_t smmu_ps_encoding(uint8_t bits) {
	switch (bits) {
	case 32u:
		return 0u;
	case 36u:
		return 1u;
	case 40u:
		return 2u;
	case 42u:
		return 3u;
	case 44u:
		return 4u;
	default:
		return 5u;
	}
}

static bool smmu_source_valid(const struct hal_iommu_controller_state* controller, uint32_t source_id) {
	return controller != NULL && controller->initialized && source_id < (1u << controller->source_id_bits);
}

static uint64_t* smmu_stream_entry(struct hal_iommu_controller_state* controller, uint32_t source_id, bool allocate) {
	if (!controller->stream_table_two_level)
		return (uint64_t*)((uint8_t*)iommu_pt_phys_to_virt(controller->stream_table_address) +
		                   (size_t)source_id * SMMU_STE_SIZE);
	uint64_t* l1    = iommu_pt_phys_to_virt(controller->stream_table_address);
	uint32_t  index = source_id >> controller->stream_table_split;
	uint64_t  entry = l1[index];
	if ((entry & ~0x3full) == 0u) {
		if (!allocate) return NULL;
		struct pmm_extent l2   = {0};
		size_t            size = controller->table_allocation_size;
		if (!iommu_pt_allocate_table(size, &l2) || ((uint64_t)l2.address & ~controller->address_mask) != 0u) {
			if (l2.size != 0u) (void)pmm_free(l2);
			return NULL;
		}
		entry     = (l2.address & ~0x3full) | controller->stream_table_split;
		l1[index] = entry;
		__asm__ volatile("dsb oshst" : : : "memory");
	}
	return (uint64_t*)((uint8_t*)iommu_pt_phys_to_virt((uintptr_t)(entry & ~0x3full)) +
	                   (size_t)(source_id & ((1u << controller->stream_table_split) - 1u)) * SMMU_STE_SIZE);
}

static void smmu_reclaim_stream_table(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	if (!controller->stream_table_two_level) return;
	uint64_t* l1    = iommu_pt_phys_to_virt(controller->stream_table_address);
	uint32_t  index = source_id >> controller->stream_table_split;
	uint64_t  entry = l1[index];
	if ((entry & ~0x3full) == 0u) return;
	uint64_t* l2 = iommu_pt_phys_to_virt((uintptr_t)(entry & ~0x3full));
	for (size_t stream = 0u; stream < (1u << controller->stream_table_split); stream++)
		if ((l2[stream * (SMMU_STE_SIZE / sizeof(uint64_t))] & SMMU_STE_VALID) != 0u) return;
	l1[index] = 0u;
	__asm__ volatile("dsb oshst" : : : "memory");
	if (!smmu_queue(controller,
	                (struct smmu_command){
						.word = {SMMU_CMD_CFGI_ALL, 31u}
    }) ||
	    !smmu_complete(controller)) {
		l1[index] = entry;
		__asm__ volatile("dsb oshst" : : : "memory");
		return;
	}
	(void)pmm_free((struct pmm_extent){(uintptr_t)(entry & ~0x3full), controller->table_allocation_size});
}

bool aarch64_smmuv3_controller_init(struct hal_iommu_controller_state*            controller,
                                    const struct hal_iommu_controller_descriptor* descriptor,
                                    struct hal_iommu_info*                        out_info) {
	const struct pmm_info* pmm     = pmm_info();
	struct pmm_extent      streams = {0}, commands = {0}, events = {0};
	if (controller == NULL || descriptor == NULL || out_info == NULL || pmm == NULL ||
	    descriptor->kind != HAL_IOMMU_KIND_ARM_SMMUV3 || descriptor->register_address == 0u)
		return false;
	*controller = (struct hal_iommu_controller_state){.registers = iommu_pt_phys_to_virt(descriptor->register_address)};
	uint32_t idr0 = smmu_read32(controller, SMMU_IDR0), idr1 = smmu_read32(controller, SMMU_IDR1);
	uint32_t idr5 = smmu_read32(controller, SMMU_IDR5), table_endian = (idr0 >> 21u) & 3u;
	uint8_t  physical_bits = smmu_oas_bits(idr5);
	if ((idr0 & 1u) == 0u || ((idr0 >> 2u) & 3u) != 2u || (idr0 & (1u << 4u)) == 0u ||
	    (table_endian != 0u && table_endian != 2u) || (idr5 & (1u << 4u)) == 0u || physical_bits == 0u ||
	    (idr1 & ((1u << 30u) | (1u << 29u))) != 0u)
		return false;
	if (physical_bits > 48u) physical_bits = 48u;
	uint8_t sid_bits = idr1 & 0x3fu;
	if (sid_bits == 0u || sid_bits > 24u) return false;
	bool   two_level   = ((idr0 >> 27u) & 3u) == 1u && sid_bits > SMMU_L2_STREAM_BITS;
	size_t stream_size = two_level ? ((size_t)1u << (sid_bits - SMMU_L2_STREAM_BITS)) * sizeof(uint64_t)
	                               : ((size_t)1u << sid_bits) * SMMU_STE_SIZE;
	if (stream_size < pmm->allocation_granule) stream_size = pmm->allocation_granule;
	size_t command_size = (size_t)SMMU_COMMAND_SIZE << SMMU_QUEUE_LOG2_ENTRIES;
	size_t event_size   = (size_t)SMMU_EVENT_SIZE << SMMU_QUEUE_LOG2_ENTRIES;
	if (command_size < pmm->allocation_granule) command_size = pmm->allocation_granule;
	if (event_size < pmm->allocation_granule) event_size = pmm->allocation_granule;
	if (!iommu_pt_allocate_table(stream_size, &streams) || !iommu_pt_allocate_table(command_size, &commands) ||
	    !iommu_pt_allocate_table(event_size, &events))
		goto fail;
	uint64_t address_mask = ((1ull << physical_bits) - 1u) & ~0xfffull;
	if (((streams.address | commands.address | events.address) & ~address_mask) != 0u) goto fail;
	controller->stream_table_address       = streams.address;
	controller->stream_table_size          = streams.size;
	controller->command_queue_address      = commands.address;
	controller->command_queue_size         = commands.size;
	controller->event_queue_address        = events.address;
	controller->event_queue_size           = events.size;
	controller->table_allocation_size      = pmm->allocation_granule > 16384u ? pmm->allocation_granule : 16384u;
	controller->leaf_size_mask             = (1ull << 12u) | (1ull << 21u) | (1ull << 30u);
	controller->address_mask               = address_mask;
	controller->io_address_bits            = 48u;
	controller->physical_address_bits      = physical_bits;
	controller->context_id_bits            = (idr0 & (1u << 18u)) != 0u ? 16u : 8u;
	controller->source_id_bits             = sid_bits;
	controller->command_queue_log2_entries = SMMU_QUEUE_LOG2_ENTRIES;
	controller->event_queue_log2_entries   = SMMU_QUEUE_LOG2_ENTRIES;
	controller->stream_table_split         = SMMU_L2_STREAM_BITS;
	controller->stream_table_two_level     = two_level;
	if (!smmu_update_cr0(controller, 0u)) goto fail;
	smmu_write32(controller, SMMU_CR1, (3u << 10u) | (1u << 8u) | (1u << 6u) | (3u << 4u) | (1u << 2u) | 1u);
	smmu_write32(controller, SMMU_CR2, 0u);
	smmu_write64(controller, SMMU_STRTAB_BASE, streams.address | SMMU_BASE_RA);
	smmu_write32(
		controller, SMMU_STRTAB_BASE_CFG, (two_level ? (1u << 16u) | (SMMU_L2_STREAM_BITS << 6u) : 0u) | sid_bits);
	smmu_write64(controller, SMMU_CMDQ_BASE, commands.address | SMMU_BASE_RA | SMMU_QUEUE_LOG2_ENTRIES);
	smmu_write32(controller, SMMU_CMDQ_PROD, 0u);
	smmu_write32(controller, SMMU_CMDQ_CONS, 0u);
	smmu_write64(controller, SMMU_EVTQ_BASE, events.address | SMMU_BASE_RA | SMMU_QUEUE_LOG2_ENTRIES);
	smmu_write32(controller, SMMU_EVTQ_PROD, 0u);
	smmu_write32(controller, SMMU_EVTQ_CONS, 0u);
	smmu_write32(controller, SMMU_GBPA, SMMU_GBPA_UPDATE | SMMU_GBPA_ABORT);
	for (size_t attempt = 0u;
	     attempt < SMMU_WAIT_LIMIT && (smmu_read32(controller, SMMU_GBPA) & SMMU_GBPA_UPDATE) != 0u;
	     attempt++)
		__asm__ volatile("yield" : : : "memory");
	if ((smmu_read32(controller, SMMU_GBPA) & SMMU_GBPA_UPDATE) != 0u ||
	    !smmu_update_cr0(controller, SMMU_CR0_CMDQEN | SMMU_CR0_EVTQEN | SMMU_CR0_SMMUEN))
		goto fail;
	if (!smmu_queue(controller,
	                (struct smmu_command){
						.word = {SMMU_CMD_CFGI_ALL, 31u}
    }) ||
	    !smmu_complete(controller))
		goto fail;
	controller->initialized = true;
	*out_info               = (struct hal_iommu_info){HAL_IOMMU_KIND_ARM_SMMUV3,
	                                                  4096u,
	                                                  controller->leaf_size_mask,
	                                                  48u,
	                                                  physical_bits,
	                                                  controller->context_id_bits,
	                                                  sid_bits};
	return true;
fail:
	(void)smmu_update_cr0(controller, 0u);
	if (events.size != 0u) (void)pmm_free(events);
	if (commands.size != 0u) (void)pmm_free(commands);
	if (streams.size != 0u) (void)pmm_free(streams);
	*controller = (struct hal_iommu_controller_state){0};
	return false;
}

void aarch64_smmuv3_controller_deinit(struct hal_iommu_controller_state* controller) {
	if (controller == NULL || !controller->initialized || controller->stream_count != 0u) return;
	smmu_lock(controller);
	(void)smmu_update_cr0(controller, 0u);
	controller->initialized = false;
	if (controller->stream_table_two_level) {
		uint64_t* l1    = iommu_pt_phys_to_virt(controller->stream_table_address);
		size_t    count = (size_t)1u << (controller->source_id_bits - controller->stream_table_split);
		for (size_t i = 0u; i < count; i++)
			if ((l1[i] & ~0x3full) != 0u)
				(void)pmm_free((struct pmm_extent){(uintptr_t)(l1[i] & ~0x3full), controller->table_allocation_size});
	}
	(void)pmm_free((struct pmm_extent){controller->event_queue_address, controller->event_queue_size});
	(void)pmm_free((struct pmm_extent){controller->command_queue_address, controller->command_queue_size});
	(void)pmm_free((struct pmm_extent){controller->stream_table_address, controller->stream_table_size});
	smmu_unlock(controller);
}

bool aarch64_smmuv3_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access) {
	if (controller == NULL || !controller->initialized) return false;
	struct iommu_pt_format format = smmu_format(controller);
	return iommu_pt_access_supported(&format, access);
}

bool aarch64_smmuv3_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                               struct hal_iommu_space_state* space) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    (controller->context_id_bits < 32u && context_id >= (1u << controller->context_id_bits)))
		return false;
	smmu_lock(controller);
	struct iommu_pt_format format = smmu_format(controller);
	bool                   ok     = iommu_pt_space_init(&format, (uintptr_t)controller, context_id, space);
	smmu_unlock(controller);
	return ok;
}

void aarch64_smmuv3_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space) {
	if (controller == NULL || space == NULL || space->table.controller_identity != (uintptr_t)controller) return;
	smmu_lock(controller);
	struct iommu_pt_format format = smmu_format(controller);
	iommu_pt_space_deinit(&format, space);
	smmu_unlock(controller);
}

bool aarch64_smmuv3_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                        const struct hal_iommu_map_request* request) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	smmu_lock(controller);
	struct iommu_pt_format format = smmu_format(controller);
	bool                   ok     = iommu_pt_map(&format, space, request, smmu_sync, controller);
	smmu_unlock(controller);
	return ok;
}

bool aarch64_smmuv3_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                          uint64_t io_address, size_t size) {
	if (controller == NULL || !controller->initialized || space == NULL ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	smmu_lock(controller);
	struct iommu_pt_format format = smmu_format(controller);
	bool                   ok     = iommu_pt_unmap(&format, space, io_address, size, smmu_sync, controller);
	smmu_unlock(controller);
	return ok;
}

bool aarch64_smmuv3_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                           uint32_t source_id) {
	if (!smmu_source_valid(controller, source_id) || space == NULL || !space->table.initialized ||
	    space->table.controller_identity != (uintptr_t)controller)
		return false;
	smmu_lock(controller);
	uint64_t* ste = smmu_stream_entry(controller, source_id, true);
	if (ste == NULL) {
		smmu_unlock(controller);
		return false;
	}
	uint64_t vtcr = 16u | (2ull << 6u) | (1ull << 8u) | (1ull << 10u) | (3ull << 12u) |
	                ((uint64_t)smmu_ps_encoding(controller->physical_address_bits) << 16u);
	uint64_t word0 = SMMU_STE_VALID | SMMU_STE_CFG_S2;
	uint64_t word2 = space->table.context_id | (vtcr << 32u) | (1ull << 51u) | (1ull << 54u);
	uint64_t word3 = space->table.root_address & controller->address_mask;
	if ((ste[0] & SMMU_STE_VALID) != 0u) {
		bool same = ste[0] == word0 && ste[2] == word2 && ste[3] == word3;
		smmu_unlock(controller);
		return same;
	}
	ste[1] = 0u;
	ste[2] = word2;
	ste[3] = word3;
	for (size_t i = 4u; i < 8u; i++) ste[i] = 0u;
	__asm__ volatile("dsb oshst" : : : "memory");
	ste[0] = word0;
	__asm__ volatile("dsb oshst" : : : "memory");
	if (!smmu_invalidate_stream(controller, source_id)) {
		ste[0] = 0u;
		(void)smmu_invalidate_stream(controller, source_id);
		smmu_reclaim_stream_table(controller, source_id);
		smmu_unlock(controller);
		return false;
	}
	controller->stream_count++;
	smmu_unlock(controller);
	return true;
}

bool aarch64_smmuv3_detach(struct hal_iommu_controller_state* controller, uint32_t source_id) {
	if (!smmu_source_valid(controller, source_id)) return false;
	smmu_lock(controller);
	uint64_t* ste = smmu_stream_entry(controller, source_id, false);
	if (ste == NULL || (ste[0] & SMMU_STE_VALID) == 0u) {
		smmu_unlock(controller);
		return false;
	}
	ste[0] = 0u;
	__asm__ volatile("dsb oshst" : : : "memory");
	bool ok = smmu_invalidate_stream(controller, source_id);
	if (ok) {
		memset(ste, 0, SMMU_STE_SIZE);
		controller->stream_count--;
		smmu_reclaim_stream_table(controller, source_id);
	}
	smmu_unlock(controller);
	return ok;
}
