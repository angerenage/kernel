#include "redirect.h"

#include <core/cpu.h>
#include <core/pmm.h>
#include <hal/interrupts.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "avec.h"
#include "controller.h"

#define REDIRECT_REGISTER_BASE 0x1fe015e0u
#define REDIRECT_NODE_SHIFT 44u
#define REDIRECT_NODE_LIMIT 16u
#define REDIRECT_CFG 0x00u
#define REDIRECT_TABLE_BASE 0x08u
#define REDIRECT_QUEUE_BASE 0x10u
#define REDIRECT_QUEUE_HEAD 0x18u
#define REDIRECT_QUEUE_TAIL 0x1cu
#define REDIRECT_REGISTER_SIZE 0x20u
#define REDIRECT_DISABLE_IDLE 2u
#define REDIRECT_TABLE_ENTRY_COUNT 65536u
#define REDIRECT_TABLE_ENTRY_SIZE 16u
#define REDIRECT_TABLE_SIZE (REDIRECT_TABLE_ENTRY_COUNT * REDIRECT_TABLE_ENTRY_SIZE)
#define REDIRECT_QUEUE_ENTRY_COUNT 4096u
#define REDIRECT_QUEUE_ENTRY_SIZE 16u
#define REDIRECT_QUEUE_SIZE (REDIRECT_QUEUE_ENTRY_COUNT * REDIRECT_QUEUE_ENTRY_SIZE)
#define REDIRECT_QUEUE_SIZE_ENCODING 0xfu
#define REDIRECT_ADDRESS_LIMIT (1ull << 48u)
#define REDIRECT_GPID_ALIGNMENT 64u
#define REDIRECT_INVALIDATION_SPINS 10000000u
#define AVEC_MESSAGE_OFFSET 0x100000u

struct redirect_entry {
	uint64_t low;
	uint64_t high;
};

struct redirect_command {
	uint64_t information;
	uint64_t notice_address;
};

struct redirect_gpid {
	uint64_t pending[4];
	uint8_t  enabled;
	uint8_t  vector;
	uint16_t reserved0;
	uint32_t target;
	uint32_t reserved1[6];
};

_Static_assert(sizeof(struct redirect_entry) == REDIRECT_TABLE_ENTRY_SIZE, "Unexpected REDIRECTINT entry size");
_Static_assert(sizeof(struct redirect_command) == REDIRECT_QUEUE_ENTRY_SIZE, "Unexpected REDIRECTINT command size");
_Static_assert(sizeof(struct redirect_gpid) == REDIRECT_GPID_ALIGNMENT, "Unexpected REDIRECTINT GPID size");

static volatile uint8_t*        redirect_registers;
static struct redirect_entry*   redirect_table;
static struct redirect_command* redirect_queue;
static volatile uint64_t*       redirect_notice;
static struct pmm_extent        redirect_table_extent;
static struct pmm_extent        redirect_queue_extent;
static struct pmm_extent        redirect_notice_extent;
static uint64_t                 redirect_allocated[REDIRECT_TABLE_ENTRY_COUNT / 64u];
static bool                     redirect_ready;

static inline uint32_t redirect_read32(uint32_t offset) {
	return *(volatile uint32_t*)(redirect_registers + offset);
}

static inline void redirect_write32(uint32_t offset, uint32_t value) {
	*(volatile uint32_t*)(redirect_registers + offset) = value;
}

static inline void redirect_write64(uint32_t offset, uint64_t value) {
	*(volatile uint64_t*)(redirect_registers + offset) = value;
}

static inline void redirect_barrier(void) {
	__asm__ volatile("dbar 0" : : : "memory");
}

static bool redirect_allocation_size(size_t required, size_t* out_size) {
	const struct pmm_info* info = pmm_info();
	if (info == NULL || info->allocation_granule == 0u || out_size == NULL) return false;
	size_t granule = info->allocation_granule;
	if (required > SIZE_MAX - (granule - 1u)) return false;
	*out_size = (required + granule - 1u) & ~(granule - 1u);
	return *out_size >= required;
}

static void* redirect_phys_to_virt(uintptr_t physical) {
	struct kernel_boot_address_space address_space;
	if (!kernel_boot_address_space_get(&address_space) || physical > UINTPTR_MAX - address_space.direct_map_offset)
		return NULL;
	return (void*)(physical + address_space.direct_map_offset);
}

static bool redirect_allocate_extent(size_t required, size_t alignment, struct pmm_extent* out_extent,
                                     void** out_virtual) {
	size_t size;
	if (out_extent == NULL || out_virtual == NULL || !redirect_allocation_size(required, &size)) return false;
	const struct pmm_info* info = pmm_info();
	if (alignment < info->allocation_granule) alignment = info->allocation_granule;
	if (!pmm_alloc(&(const struct pmm_alloc_request){.size            = size,
	                                                 .alignment       = alignment,
	                                                 .maximum_address = REDIRECT_ADDRESS_LIMIT},
	               out_extent))
		return false;
	*out_virtual = redirect_phys_to_virt(out_extent->address);
	if (*out_virtual != NULL) {
		memset(*out_virtual, 0, out_extent->size);
		return true;
	}
	(void)pmm_free(*out_extent);
	*out_extent = (struct pmm_extent){0};
	return false;
}

static void redirect_release_controller_storage(void) {
	if (redirect_notice_extent.size != 0u) (void)pmm_free(redirect_notice_extent);
	if (redirect_queue_extent.size != 0u) (void)pmm_free(redirect_queue_extent);
	if (redirect_table_extent.size != 0u) (void)pmm_free(redirect_table_extent);
	redirect_notice_extent = (struct pmm_extent){0};
	redirect_queue_extent  = (struct pmm_extent){0};
	redirect_table_extent  = (struct pmm_extent){0};
	redirect_notice        = NULL;
	redirect_queue         = NULL;
	redirect_table         = NULL;
}

static bool redirect_controller_init(void) {
	if (redirect_ready) return true;
	uint32_t node = eiointc.described ? eiointc.node : 0u;
	if (node >= REDIRECT_NODE_LIMIT) return false;
	uintptr_t register_base = REDIRECT_REGISTER_BASE | ((uintptr_t)node << REDIRECT_NODE_SHIFT);
	void*     table;
	void*     queue;
	void*     notice;
	if (!loongarch64_map_mmio(register_base, REDIRECT_REGISTER_SIZE, &redirect_registers) ||
	    !redirect_allocate_extent(REDIRECT_TABLE_SIZE, REDIRECT_TABLE_SIZE, &redirect_table_extent, &table) ||
	    !redirect_allocate_extent(REDIRECT_QUEUE_SIZE, REDIRECT_QUEUE_SIZE, &redirect_queue_extent, &queue) ||
	    !redirect_allocate_extent(sizeof(uint64_t), REDIRECT_GPID_ALIGNMENT, &redirect_notice_extent, &notice)) {
		redirect_release_controller_storage();
		return false;
	}
	redirect_table  = table;
	redirect_queue  = queue;
	redirect_notice = notice;
	memset(redirect_allocated, 0, sizeof(redirect_allocated));
	redirect_write64(REDIRECT_CFG, REDIRECT_DISABLE_IDLE);
	redirect_write64(REDIRECT_TABLE_BASE, redirect_table_extent.address);
	redirect_write32(REDIRECT_QUEUE_HEAD, 0u);
	redirect_write32(REDIRECT_QUEUE_TAIL, 0u);
	redirect_write64(REDIRECT_QUEUE_BASE,
	                 (redirect_queue_extent.address & 0x0000fffffffff000ull) | REDIRECT_QUEUE_SIZE_ENCODING);
	redirect_barrier();
	redirect_ready = true;
	return true;
}

static bool redirect_index_allocate(uint32_t* out_index) {
	if (out_index == NULL) return false;
	for (uint32_t word = 0u; word < REDIRECT_TABLE_ENTRY_COUNT / 64u; word++) {
		uint64_t available = ~redirect_allocated[word];
		if (available == 0u) continue;
		uint32_t bit = (uint32_t)__builtin_ctzll(available);
		redirect_allocated[word] |= 1ull << bit;
		*out_index = word * 64u + bit;
		return true;
	}
	return false;
}

static void redirect_index_release(uint32_t index) {
	if (index >= REDIRECT_TABLE_ENTRY_COUNT) return;
	redirect_allocated[index / 64u] &= ~(1ull << (index % 64u));
}

static bool redirect_invalidate(uint32_t index) {
	if (!redirect_ready || index >= REDIRECT_TABLE_ENTRY_COUNT) return false;
	uint32_t head = redirect_read32(REDIRECT_QUEUE_HEAD);
	uint32_t tail = redirect_read32(REDIRECT_QUEUE_TAIL);
	uint32_t next = (tail + 1u) % REDIRECT_QUEUE_ENTRY_COUNT;
	if (head >= REDIRECT_QUEUE_ENTRY_COUNT || tail >= REDIRECT_QUEUE_ENTRY_COUNT || head == next) return false;
	*redirect_notice     = 0u;
	redirect_queue[tail] = (struct redirect_command){.information    = (1ull << 5u) | ((uint64_t)index << 8u),
	                                                 .notice_address = redirect_notice_extent.address};
	redirect_barrier();
	redirect_write32(REDIRECT_QUEUE_TAIL, next);
	for (uint32_t spin = 0u; spin < REDIRECT_INVALIDATION_SPINS; spin++) {
		if (__atomic_load_n(redirect_notice, __ATOMIC_ACQUIRE) != 0u) return true;
		__asm__ volatile("nop");
	}
	return false;
}

bool loongarch64_redirect_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                           const struct cpu* target) {
	if (!loongarch64_avec_uses_redirect() || domain != LOONGARCH64_MESSAGE_DOMAIN_REDIRECT || source != NULL ||
	    !loongarch64_avec_cpu_supported(target))
		return false;
	return true;
}

bool loongarch64_redirect_init(struct hal_interrupt_message_state*         state,
                               const struct hal_interrupt_message_request* request,
                               struct hal_interrupt_message*               out_message) {
	struct hal_interrupt_message_range range;
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !loongarch64_redirect_target_supported(request->domain, request->source, request->target) ||
	    !loongarch64_avec_range(&range) || request->event.domain != range.delivery.domain ||
	    request->event.id < range.delivery.base || request->event.id >= range.delivery.limit)
		return false;
	struct irq_state  irq             = loongarch64_controllers_lock();
	uint32_t          index           = 0u;
	bool              index_allocated = false;
	struct pmm_extent gpid_extent     = {0};
	void*             gpid_virtual;
	if (!redirect_controller_init() || !(index_allocated = redirect_index_allocate(&index)) ||
	    !redirect_allocate_extent(sizeof(struct redirect_gpid), REDIRECT_GPID_ALIGNMENT, &gpid_extent, &gpid_virtual)) {
		if (gpid_extent.size != 0u) (void)pmm_free(gpid_extent);
		if (index_allocated) redirect_index_release(index);
		loongarch64_controllers_unlock(irq);
		return false;
	}
	struct redirect_gpid* gpid = gpid_virtual;
	gpid->enabled              = 1u;
	gpid->vector               = (uint8_t)request->event.id;
	gpid->target               = (uint32_t)request->target->index;
	redirect_table[index] =
		(struct redirect_entry){.low = 1ull | (gpid_extent.address & 0x0000ffffffffffc0ull) | (0xffull << 56u)};
	redirect_barrier();
	loongarch64_controllers_unlock(irq);
	*out_message =
		(struct hal_interrupt_message){.address = (pch_msi.address - AVEC_MESSAGE_OFFSET) | (1u << 2u), .data = index};
	*state = (struct hal_interrupt_message_state){.domain                = LOONGARCH64_MESSAGE_DOMAIN_REDIRECT,
	                                              .event                 = request->event,
	                                              .redirect_gpid_address = gpid_extent.address,
	                                              .redirect_gpid_size    = gpid_extent.size,
	                                              .redirect_index        = index,
	                                              .initialized           = true};
	return true;
}

bool loongarch64_redirect_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->domain != LOONGARCH64_MESSAGE_DOMAIN_REDIRECT ||
	    state->event.domain != LOONGARCH64_DELIVERY_DOMAIN_AVEC ||
	    state->redirect_index >= REDIRECT_TABLE_ENTRY_COUNT || state->redirect_gpid_size == 0u)
		return false;
	struct redirect_gpid* gpid = redirect_phys_to_virt(state->redirect_gpid_address);
	if (gpid == NULL) return false;
	struct irq_state irq = loongarch64_controllers_lock();
	gpid->enabled        = 0u;
	redirect_barrier();
	redirect_table[state->redirect_index] = (struct redirect_entry){0};
	redirect_barrier();
	if (!redirect_invalidate(state->redirect_index) ||
	    !pmm_free((struct pmm_extent){.address = state->redirect_gpid_address, .size = state->redirect_gpid_size})) {
		loongarch64_controllers_unlock(irq);
		return false;
	}
	redirect_index_release(state->redirect_index);
	loongarch64_controllers_unlock(irq);
	*state = (struct hal_interrupt_message_state){0};
	return true;
}
