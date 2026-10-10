#include "plic.h"

#include <boot/info.h>
#include <core/cpu.h>
#include <core/interrupt.h>
#include <firmware/dt/device.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "interrupts.h"

#define PLIC_MAX_CPUS 64u
#define PLIC_MAX_SOURCES 1023u
#define PLIC_PAGE_SIZE 0x1000u
#define PLIC_PRIORITY_BASE 0x000000u
#define PLIC_ENABLE_BASE 0x002000u
#define PLIC_ENABLE_STRIDE 0x80u
#define PLIC_CONTEXT_BASE 0x200000u
#define PLIC_CONTEXT_STRIDE 0x1000u
#define PLIC_CONTEXT_CLAIM 0x4u
#define PLIC_SUPERVISOR_EXTERNAL 9u

struct plic_context {
	uint64_t hart_id;
	uint32_t number;
};

struct plic_discovery {
	struct dt_node      node;
	uintptr_t           physical_base;
	uint64_t            size;
	uint32_t            ndev;
	struct plic_context contexts[PLIC_MAX_CPUS];
	size_t              context_count;
};

static struct {
	uintptr_t           physical_base;
	uintptr_t           virtual_base;
	uint64_t            size;
	uint32_t            ndev;
	struct plic_context contexts[PLIC_MAX_CPUS];
	size_t              context_count;
	bool                ready;
} plic;
static uint32_t plic_init_lock;
static uint32_t plic_register_lock;

bool riscv64_plic_device_tree_interrupt(const uint32_t* cells, size_t cell_count, uint32_t* out_local_source_id,
                                        enum hal_interrupt_trigger*  out_trigger,
                                        enum hal_interrupt_polarity* out_polarity) {
	if (cells == NULL || cell_count != 1u || out_local_source_id == NULL || out_trigger == NULL ||
	    out_polarity == NULL || cells[0] == 0u)
		return false;
	*out_local_source_id = cells[0];
	*out_trigger         = HAL_INTERRUPT_TRIGGER_FIRMWARE;
	*out_polarity        = HAL_INTERRUPT_POLARITY_FIRMWARE;
	return true;
}

static bool plic_dt_node(struct dt_node* out) {
	static const char* const compatibles[] = {"sifive,plic-1.0.0", "riscv,plic0"};
	struct dt_node           found         = DT_NODE_INVALID;

	if (out == NULL) return false;
	for (size_t compatible = 0u; compatible < sizeof(compatibles) / sizeof(compatibles[0]); compatible++) {
		size_t count = dt_device_count(compatibles[compatible]);

		for (size_t index = 0u; index < count; index++) {
			struct dt_node node = dt_device_at(compatibles[compatible], index);

			if (dt_node_valid(found) && found.id != node.id) return false;
			found = node;
		}
	}
	if (!dt_node_valid(found)) return false;
	*out = found;
	return true;
}

static bool plic_fdt_find(struct plic_discovery* out) {
	struct dt_node     node;
	struct dt_property interrupts;
	struct dt_property ndev;
	struct dt_reg      reg;
	uint64_t           ndev_value;

	if (out == NULL || !plic_dt_node(&node) || !dt_node_reg(node, 0u, &reg) || reg.address > UINTPTR_MAX ||
	    !dt_node_property(node, "riscv,ndev", &ndev) || ndev.size != 4u ||
	    !dt_property_read_cells(&ndev, 0u, 1u, &ndev_value) || ndev_value == 0u || ndev_value > PLIC_MAX_SOURCES ||
	    !dt_node_property(node, "interrupts-extended", &interrupts) || interrupts.size % 8u != 0u)
		return false;
	*out = (struct plic_discovery){
		.node          = node,
		.physical_base = (uintptr_t)reg.address,
		.size          = reg.size,
		.ndev          = (uint32_t)ndev_value,
	};
	for (size_t entry = 0u; entry < interrupts.size / 8u; entry++) {
		uint64_t phandle;
		uint64_t cause;
		uint64_t hart_id;

		if (!dt_property_read_cells(&interrupts, entry * 2u, 1u, &phandle) ||
		    !dt_property_read_cells(&interrupts, entry * 2u + 1u, 1u, &cause) ||
		    !riscv64_interrupt_hart_for_phandle((uint32_t)phandle, &hart_id) ||
		    (cause != 11u && cause != PLIC_SUPERVISOR_EXTERNAL && cause != UINT32_MAX))
			return false;
		if (cause != PLIC_SUPERVISOR_EXTERNAL) continue;
		if (out->context_count == PLIC_MAX_CPUS) return false;
		for (size_t index = 0u; index < out->context_count; index++)
			if (out->contexts[index].hart_id == hart_id) return false;
		out->contexts[out->context_count++] = (struct plic_context){.hart_id = hart_id, .number = (uint32_t)entry};
	}
	return out->context_count != 0u;
}

size_t riscv64_plic_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	struct plic_discovery found;

	if (!plic_fdt_find(&found)) return 0u;
	if (nodes != NULL && capacity != 0u) {
		nodes[0] = (struct hal_device_tree_consumed_node){
			.node            = found.node,
			.kind            = HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER,
			.value           = found.physical_base,
			.specifier_cells = 1u,
		};
	}
	return 1u;
}

static bool plic_map_page(uintptr_t physical, uintptr_t direct_map_offset) {
	uintptr_t page = physical & ~(uintptr_t)(PLIC_PAGE_SIZE - 1u);
	if (direct_map_offset > UINTPTR_MAX - page) return false;
	uintptr_t virtual_page = direct_map_offset + page;
	if (hal_paging_query(hal_paging_kernel_space(), virtual_page, NULL)) return true;
	uint64_t flags = HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL;
	/* Without Svpbmt, the PTE retains the physical address's platform memory attributes. */
	enum memory_type type =
		hal_paging_mapping_supported(flags, MEMORY_TYPE_DEVICE) ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL;
	return hal_paging_map(hal_paging_kernel_space(),
	                      &(const struct hal_paging_map_request){virtual_page, page, PLIC_PAGE_SIZE, flags, type});
}

static bool plic_map_offset(const struct plic_discovery* found, uint64_t offset, uintptr_t direct_map_offset) {
	if (offset > found->size || found->size - offset < 4u || offset > UINTPTR_MAX - found->physical_base) return false;
	return plic_map_page(found->physical_base + (uintptr_t)offset, direct_map_offset);
}

static bool plic_probe(void) {
	if (__atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE)) return true;
	struct irq_state irq = irq_save_disable();
	while (__atomic_exchange_n(&plic_init_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop");
	if (__atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE)) goto done;
	struct boot_address_space address_space;
	struct plic_discovery     found;
	if (!boot_address_space_get(&address_space) || !plic_fdt_find(&found) ||
	    !plic_map_offset(&found, PLIC_PRIORITY_BASE + PLIC_MAX_SOURCES * 4u, address_space.direct_map_offset))
		goto done;
	for (size_t index = 0u; index < found.context_count; index++) {
		uint64_t context = found.contexts[index].number;
		uint64_t enable  = PLIC_ENABLE_BASE + context * PLIC_ENABLE_STRIDE + (found.ndev / 32u) * 4u;
		uint64_t claim   = PLIC_CONTEXT_BASE + context * PLIC_CONTEXT_STRIDE + PLIC_CONTEXT_CLAIM;
		if (!plic_map_offset(&found, enable, address_space.direct_map_offset) ||
		    !plic_map_offset(&found, claim, address_space.direct_map_offset))
			goto done;
	}
	if (found.physical_base > UINTPTR_MAX - address_space.direct_map_offset) goto done;
	plic.physical_base = found.physical_base;
	plic.virtual_base  = found.physical_base + address_space.direct_map_offset;
	plic.size          = found.size;
	plic.ndev          = found.ndev;
	plic.context_count = found.context_count;
	memcpy(plic.contexts, found.contexts, found.context_count * sizeof(found.contexts[0]));
	for (size_t index = 0u; index < plic.context_count; index++) {
		uint64_t enable = PLIC_ENABLE_BASE + (uint64_t)plic.contexts[index].number * PLIC_ENABLE_STRIDE;
		for (uint32_t word = 0u; word <= plic.ndev / 32u; word++)
			*(volatile uint32_t*)(plic.virtual_base + (uintptr_t)enable + word * 4u) = 0u;
		uint64_t offset = PLIC_CONTEXT_BASE + (uint64_t)plic.contexts[index].number * PLIC_CONTEXT_STRIDE;
		*(volatile uint32_t*)(plic.virtual_base + (uintptr_t)offset) = 0u;
	}
	__atomic_store_n(&plic.ready, true, __ATOMIC_RELEASE);
done:
	__atomic_store_n(&plic_init_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(irq);
	return __atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE);
}

static bool plic_context_for_cpu(const struct cpu* cpu, uint32_t* out_context) {
	if (cpu == NULL || out_context == NULL || !__atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE)) return false;
	for (size_t index = 0u; index < plic.context_count; index++) {
		if (plic.contexts[index].hart_id != cpu->arch_id) continue;
		*out_context = plic.contexts[index].number;
		return true;
	}
	return false;
}

static inline volatile uint32_t* plic_register(uint64_t offset) {
	return (volatile uint32_t*)(plic.virtual_base + (uintptr_t)offset);
}

static bool plic_set_source_enabled(uint32_t context, uint32_t source, bool enabled) {
	if (!__atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE) || source == 0u || source > plic.ndev) return false;
	uint64_t offset = PLIC_ENABLE_BASE + (uint64_t)context * PLIC_ENABLE_STRIDE + (source / 32u) * 4u;
	if (offset > plic.size || plic.size - offset < 4u) return false;
	struct irq_state irq = irq_save_disable();
	while (__atomic_exchange_n(&plic_register_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop");
	uint32_t value = *plic_register(offset);
	if (enabled) value |= 1u << (source % 32u);
	else value &= ~(1u << (source % 32u));
	*plic_register(offset) = value;
	__atomic_store_n(&plic_register_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(irq);
	return true;
}

bool riscv64_plic_source_domain_at(struct hal_interrupt_source_domain_info* out_domain) {
	if (out_domain == NULL || !plic_probe()) return false;
	*out_domain =
		(struct hal_interrupt_source_domain_info){.domain = 1u, .first_source = 1u, .source_count = plic.ndev};
	return true;
}

bool riscv64_plic_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                 struct hal_interrupt_source* out_source) {
	if (out_source == NULL || !plic_probe() || controller_address != plic.physical_base || local_source_id == 0u ||
	    local_source_id > plic.ndev)
		return false;
	*out_source = (struct hal_interrupt_source){.domain = 1u, .number = local_source_id};
	return true;
}

bool riscv64_plic_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out_info) {
	if (source == NULL || out_info == NULL || source->domain != 1u || !plic_probe() || source->number == 0u ||
	    source->number > plic.ndev)
		return false;
	*out_info = (struct hal_interrupt_source_info){
		.delivery     = {.domain = RISCV64_DELIVERY_DOMAIN_PLIC, .base = source->number, .limit = source->number + 1u},
		.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
		.fixed_target = NULL
    };
	return true;
}

bool riscv64_plic_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	uint32_t context;
	return source != NULL && source->domain == 1u && __atomic_load_n(&plic.ready, __ATOMIC_ACQUIRE) &&
	       source->number != 0u && source->number <= plic.ndev && plic_context_for_cpu(target, &context);
}

bool riscv64_plic_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                              const struct hal_interrupt_delivery* delivery) {
	struct hal_interrupt_source_info info;
	uint32_t                         context;
	if (state == NULL || state->initialized || delivery == NULL || delivery->target == NULL ||
	    delivery->trigger != HAL_INTERRUPT_TRIGGER_FIRMWARE || delivery->polarity != HAL_INTERRUPT_POLARITY_FIRMWARE ||
	    !riscv64_plic_source_info(source, &info) || delivery->event.domain != info.delivery.domain ||
	    delivery->event.id != info.delivery.base || !riscv64_plic_source_target_supported(source, delivery->target) ||
	    !plic_context_for_cpu(delivery->target, &context))
		return false;
	for (size_t index = 0u; index < plic.context_count; index++) {
		if (!plic_set_source_enabled(plic.contexts[index].number, source->number, false)) return false;
	}
	*plic_register(PLIC_PRIORITY_BASE + (uint64_t)source->number * 4u) = 1u;
	if (*plic_register(PLIC_PRIORITY_BASE + (uint64_t)source->number * 4u) == 0u) {
		*plic_register(PLIC_PRIORITY_BASE + (uint64_t)source->number * 4u) = 1u;
		if (*plic_register(PLIC_PRIORITY_BASE + (uint64_t)source->number * 4u) == 0u) return false;
	}
	*state = (struct hal_interrupt_source_state){
		.source = *source, .target = delivery->target, .route = context, .initialized = true, .masked = true};
	return true;
}

bool riscv64_plic_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || !plic_set_source_enabled(state->route, state->source.number, false))
		return false;
	state->masked = true;
	if (state->target == cpu_current()) riscv64_sync_external_interrupt_local();
	else hal_cpu_kick(state->target);
	return true;
}

bool riscv64_plic_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || !plic_set_source_enabled(state->route, state->source.number, true))
		return false;
	state->masked = false;
	if (state->target == cpu_current()) riscv64_sync_external_interrupt_local();
	else hal_cpu_kick(state->target);
	return true;
}

bool riscv64_plic_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!riscv64_plic_source_mask(state)) return false;
	state->initialized = false;
	return true;
}

bool riscv64_plic_cpu_has_enabled_sources(const struct cpu* cpu) {
	uint32_t context;
	if (!plic_context_for_cpu(cpu, &context)) return false;
	for (uint32_t word = 0u; word <= plic.ndev / 32u; word++) {
		uint64_t offset = PLIC_ENABLE_BASE + (uint64_t)context * PLIC_ENABLE_STRIDE + word * 4u;
		if (*plic_register(offset) != 0u) return true;
	}
	return false;
}

bool riscv64_plic_handle_external_irq(void) {
	uint32_t context;
	if (!plic_context_for_cpu(cpu_current(), &context)) return false;
	uint64_t claim_offset = PLIC_CONTEXT_BASE + (uint64_t)context * PLIC_CONTEXT_STRIDE + PLIC_CONTEXT_CLAIM;
	uint32_t claim        = *plic_register(claim_offset);
	if (claim == 0u) return false;
	if (claim <= plic.ndev &&
	    !interrupt_handle_event((struct hal_interrupt_event){.domain = RISCV64_DELIVERY_DOMAIN_PLIC, .id = claim})) {
		(void)plic_set_source_enabled(context, claim, false);
	}
	*plic_register(claim_offset) = claim;
	return true;
}
