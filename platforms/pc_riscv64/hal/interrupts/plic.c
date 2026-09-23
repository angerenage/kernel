#include "plic.h"

#include <core/cpu.h>
#include <core/interrupt.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_fdt.h"
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

struct plic_fdt_extra {
	uint32_t       phandle;
	bool           has_phandle;
	const uint8_t* interrupts_extended;
	size_t         interrupts_extended_size;
	uint32_t       ndev;
	bool           has_ndev;
};

struct plic_cpu_intc {
	uint32_t phandle;
	uint64_t hart_id;
};

struct plic_context {
	uint64_t hart_id;
	uint32_t number;
};

struct plic_discovery {
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

static bool plic_fdt_find(struct plic_discovery* out) {
	struct kernel_boot_data dtb;
	struct iommu_fdt_node   stack[IOMMU_FDT_MAX_DEPTH];
	struct plic_fdt_extra   extras[IOMMU_FDT_MAX_DEPTH];
	struct plic_cpu_intc    cpu_intcs[PLIC_MAX_CPUS];
	size_t                  cpu_intc_count       = 0u;
	const uint8_t*          plic_interrupts      = NULL;
	size_t                  plic_interrupts_size = 0u;
	size_t                  plic_count           = 0u;
	if (out == NULL || !kernel_boot_dtb_get(&dtb) || dtb.address == NULL || dtb.size < 40u ||
	    iommu_fdt_u32(dtb.address) != IOMMU_FDT_MAGIC)
		return false;
	const uint8_t* blob             = dtb.address;
	uint32_t       total_size       = iommu_fdt_u32(blob + 4u);
	uint32_t       structure_offset = iommu_fdt_u32(blob + 8u);
	uint32_t       strings_offset   = iommu_fdt_u32(blob + 12u);
	uint32_t       strings_size     = iommu_fdt_u32(blob + 32u);
	uint32_t       structure_size   = iommu_fdt_u32(blob + 36u);
	if (total_size > dtb.size || structure_offset > total_size || structure_size > total_size - structure_offset ||
	    strings_offset > total_size || strings_size > total_size - strings_offset)
		return false;
	const uint8_t* cursor  = blob + structure_offset;
	const uint8_t* end     = cursor + structure_size;
	const uint8_t* strings = blob + strings_offset;
	size_t         depth   = 0u;
	memset(out, 0, sizeof(*out));
	while ((size_t)(end - cursor) >= 4u) {
		uint32_t token = iommu_fdt_u32(cursor);
		cursor += 4u;
		if (token == IOMMU_FDT_BEGIN_NODE) {
			if (depth == IOMMU_FDT_MAX_DEPTH) return false;
			const uint8_t* name_end = memchr(cursor, '\0', (size_t)(end - cursor));
			if (name_end == NULL) return false;
			uint32_t parent_address_cells = depth == 0u ? 2u : stack[depth - 1u].address_cells;
			uint32_t parent_size_cells    = depth == 0u ? 1u : stack[depth - 1u].size_cells;
			stack[depth]                  = (struct iommu_fdt_node){.parent_address_cells = parent_address_cells,
			                                                        .parent_size_cells    = parent_size_cells,
			                                                        .address_cells        = 2u,
			                                                        .size_cells           = 1u};
			extras[depth]                 = (struct plic_fdt_extra){0};
			depth++;
			cursor += ((size_t)(name_end - cursor) + 4u) & ~(size_t)3u;
		}
		else if (token == IOMMU_FDT_END_NODE) {
			if (depth == 0u) return false;
			struct iommu_fdt_node* node  = &stack[depth - 1u];
			struct plic_fdt_extra* extra = &extras[depth - 1u];
			if (iommu_fdt_node_enabled(node) &&
			    iommu_fdt_string_list_contains(node->compatible, node->compatible_size, "riscv,cpu-intc")) {
				uint64_t hart_id;
				if (depth < 2u || !extra->has_phandle || extra->phandle == 0u || cpu_intc_count == PLIC_MAX_CPUS ||
				    !iommu_fdt_node_enabled(&stack[depth - 2u]) ||
				    stack[depth - 2u].reg_size < stack[depth - 2u].parent_address_cells * 4u ||
				    !iommu_fdt_cells(stack[depth - 2u].reg, stack[depth - 2u].parent_address_cells, &hart_id))
					return false;
				for (size_t index = 0u; index < cpu_intc_count; index++) {
					if (cpu_intcs[index].phandle == extra->phandle || cpu_intcs[index].hart_id == hart_id) return false;
				}
				cpu_intcs[cpu_intc_count++] = (struct plic_cpu_intc){.phandle = extra->phandle, .hart_id = hart_id};
			}
			if (iommu_fdt_node_enabled(node) &&
			    (iommu_fdt_string_list_contains(node->compatible, node->compatible_size, "sifive,plic-1.0.0") ||
			     iommu_fdt_string_list_contains(node->compatible, node->compatible_size, "riscv,plic0"))) {
				uint64_t address;
				uint64_t size;
				if (++plic_count != 1u || !extra->has_ndev || extra->ndev == 0u || extra->ndev > PLIC_MAX_SOURCES ||
				    node->parent_size_cells == 0u || node->parent_size_cells > 2u ||
				    node->reg_size < (size_t)(node->parent_address_cells + node->parent_size_cells) * 4u ||
				    !iommu_fdt_node_address(stack, depth, &address) || address > UINTPTR_MAX ||
				    !iommu_fdt_cells(node->reg + node->parent_address_cells * 4u, node->parent_size_cells, &size) ||
				    extra->interrupts_extended == NULL || extra->interrupts_extended_size == 0u)
					return false;
				out->physical_base   = (uintptr_t)address;
				out->size            = size;
				out->ndev            = extra->ndev;
				plic_interrupts      = extra->interrupts_extended;
				plic_interrupts_size = extra->interrupts_extended_size;
			}
			depth--;
		}
		else if (token == IOMMU_FDT_PROPERTY) {
			if (depth == 0u || (size_t)(end - cursor) < 8u) return false;
			uint32_t length      = iommu_fdt_u32(cursor);
			uint32_t name_offset = iommu_fdt_u32(cursor + 4u);
			cursor += 8u;
			if (length > (size_t)(end - cursor) || name_offset >= strings_size) return false;
			const char* name = (const char*)strings + name_offset;
			if (memchr(name, '\0', strings_size - name_offset) == NULL) return false;
			struct iommu_fdt_node* node  = &stack[depth - 1u];
			struct plic_fdt_extra* extra = &extras[depth - 1u];
			if (strcmp(name, "#address-cells") == 0 && length == 4u) node->address_cells = iommu_fdt_u32(cursor);
			else if (strcmp(name, "#size-cells") == 0 && length == 4u) node->size_cells = iommu_fdt_u32(cursor);
			else if (strcmp(name, "compatible") == 0) {
				node->compatible      = cursor;
				node->compatible_size = length;
			}
			else if (strcmp(name, "status") == 0) {
				node->status      = cursor;
				node->status_size = length;
			}
			else if (strcmp(name, "reg") == 0) {
				node->reg      = cursor;
				node->reg_size = length;
			}
			else if (strcmp(name, "ranges") == 0) {
				node->ranges      = cursor;
				node->ranges_size = length;
			}
			else if (strcmp(name, "phandle") == 0 && length == 4u) {
				extra->phandle     = iommu_fdt_u32(cursor);
				extra->has_phandle = true;
			}
			else if (strcmp(name, "interrupts-extended") == 0) {
				extra->interrupts_extended      = cursor;
				extra->interrupts_extended_size = length;
			}
			else if (strcmp(name, "riscv,ndev") == 0 && length == 4u) {
				extra->ndev     = iommu_fdt_u32(cursor);
				extra->has_ndev = true;
			}
			cursor += (length + 3u) & ~3u;
		}
		else if (token == IOMMU_FDT_NOP) continue;
		else if (token == IOMMU_FDT_END) break;
		else return false;
		if (cursor > end) return false;
	}
	if (plic_count != 1u || depth != 0u || plic_interrupts_size % 8u != 0u) return false;
	for (size_t offset = 0u; offset < plic_interrupts_size; offset += 8u) {
		uint32_t phandle = iommu_fdt_u32(plic_interrupts + offset);
		uint32_t cause   = iommu_fdt_u32(plic_interrupts + offset + 4u);
		uint64_t hart_id = UINT64_MAX;
		for (size_t index = 0u; index < cpu_intc_count; index++) {
			if (cpu_intcs[index].phandle == phandle) {
				hart_id = cpu_intcs[index].hart_id;
				break;
			}
		}
		if (hart_id == UINT64_MAX || (cause != 11u && cause != PLIC_SUPERVISOR_EXTERNAL && cause != UINT32_MAX))
			return false;
		if (cause != PLIC_SUPERVISOR_EXTERNAL) continue;
		if (out->context_count == PLIC_MAX_CPUS) return false;
		for (size_t index = 0u; index < out->context_count; index++)
			if (out->contexts[index].hart_id == hart_id) return false;
		out->contexts[out->context_count++] =
			(struct plic_context){.hart_id = hart_id, .number = (uint32_t)(offset / 8u)};
	}
	return out->context_count != 0u;
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
	struct kernel_boot_address_space address_space;
	struct plic_discovery            found;
	if (!kernel_boot_address_space_get(&address_space) || !plic_fdt_find(&found) ||
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
