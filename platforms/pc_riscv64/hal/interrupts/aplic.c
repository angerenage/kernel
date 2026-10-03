#include "aplic.h"

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

#include "imsic.h"
#include "interrupts.h"

#define APLIC_MAX_HARTS 64u
#define APLIC_MAX_SOURCES 1023u
#define APLIC_DOMAIN_ID 2u
#define APLIC_PAGE_SIZE 0x1000u
#define APLIC_DOMAINCFG 0x0000u
#define APLIC_DOMAINCFG_IE (1u << 8u)
#define APLIC_DOMAINCFG_DM (1u << 2u)
#define APLIC_SOURCECFG_BASE 0x0004u
#define APLIC_SOURCECFG_DELEGATED (1u << 10u)
#define APLIC_SOURCECFG_EDGE_RISE 4u
#define APLIC_SOURCECFG_EDGE_FALL 5u
#define APLIC_SOURCECFG_LEVEL_HIGH 6u
#define APLIC_SOURCECFG_LEVEL_LOW 7u
#define APLIC_SETIENUM 0x1edcu
#define APLIC_CLRIE_BASE 0x1f00u
#define APLIC_CLRIENUM 0x1fdcu
#define APLIC_SETIPNUM_LE 0x2000u
#define APLIC_TARGET_BASE 0x3004u
#define APLIC_IDC_BASE 0x4000u
#define APLIC_IDC_STRIDE 0x20u
#define APLIC_IDC_IDELIVERY 0x00u
#define APLIC_IDC_ITHRESHOLD 0x08u
#define APLIC_IDC_CLAIMI 0x1cu

struct aplic_hart {
	uint64_t hart_id;
	uint32_t index;
};

static struct {
	uintptr_t         physical_base;
	uintptr_t         virtual_base;
	uintptr_t         size;
	uint32_t          sources;
	struct aplic_hart harts[APLIC_MAX_HARTS];
	size_t            hart_count;
	bool              msi;
	bool              probed;
	bool              ready;
} aplic;
static uint32_t aplic_init_lock;

size_t riscv64_aplic_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	size_t count        = 0u;
	size_t device_count = dt_device_count("riscv,aplic");

	for (size_t index = 0u; index < device_count; index++) {
		struct dt_node node = dt_device_at("riscv,aplic", index);
		struct dt_reg  reg;

		if (!dt_node_reg(node, 0u, &reg) || reg.address == 0u || reg.address > UINTPTR_MAX || reg.size > UINTPTR_MAX)
			continue;
		if (nodes != NULL && count < capacity) {
			nodes[count] = (struct hal_device_tree_consumed_node){
				.node = node, .kind = HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER, .value = reg.address};
		}
		count++;
	}
	return count;
}

bool riscv64_interrupt_hart_for_phandle(uint32_t wanted, uint64_t* out_hart) {
	struct dt_node interrupt_controller;
	struct dt_node cpu;
	struct dt_reg  reg;

	if (wanted == 0u || out_hart == NULL) return false;
	interrupt_controller = dt_node_by_phandle(wanted);
	if (!dt_node_enabled(interrupt_controller) || !dt_node_compatible(interrupt_controller, "riscv,cpu-intc"))
		return false;
	cpu = dt_node_parent(interrupt_controller);
	if (!dt_node_enabled(cpu) || !dt_node_reg_raw(cpu, 0u, &reg)) return false;
	*out_hart = reg.address;
	return true;
}

static bool aplic_find(void) {
	size_t controller_count = dt_device_count("riscv,aplic");
	if (controller_count == 0u || controller_count > 16u) return false;
	bool selected = false;
	for (size_t controller = 0u; controller < controller_count; controller++) {
		struct dt_node     node = dt_device_at("riscv,aplic", controller);
		struct dt_property msi_parent;
		uint64_t           msi_parent_value = 0u;
		bool               msi              = dt_node_property(node, "msi-parent", &msi_parent);

		if (msi && (msi_parent.size != 4u || !dt_property_read_cells(&msi_parent, 0u, 1u, &msi_parent_value) ||
		            msi_parent_value == 0u))
			return false;
		if (msi && !riscv64_imsic_is_parent((uint32_t)msi_parent_value)) continue;
		struct dt_property interrupts;
		if (!msi) {
			if (!dt_node_property(node, "interrupts-extended", &interrupts)) continue;
			if (interrupts.size == 0u || interrupts.size % 8u != 0u || interrupts.size > APLIC_MAX_HARTS * 8u)
				return false;
			bool supervisor = false;
			for (size_t index = 0u; index < interrupts.size / 8u; index++) {
				uint64_t cause;

				if (!dt_property_read_cells(&interrupts, index * 2u + 1u, 1u, &cause)) return false;
				if (cause != 9u && cause != 11u && cause != UINT32_MAX) return false;
				if (cause == 9u) supervisor = true;
			}
			if (!supervisor) continue;
		}
		if (selected) return false;
		selected = true;
		struct dt_reg reg;
		if (!dt_node_reg(node, 0u, &reg) || reg.address == 0u || reg.address > UINTPTR_MAX || reg.size > UINTPTR_MAX)
			return false;
		uintptr_t          base = (uintptr_t)reg.address;
		uintptr_t          size = (uintptr_t)reg.size;
		struct dt_property count;
		uint64_t           sources_value;
		if (!dt_node_property(node, "riscv,num-sources", &count) || count.size != 4u ||
		    !dt_property_read_cells(&count, 0u, 1u, &sources_value))
			return false;
		uint32_t sources = (uint32_t)sources_value;
		if (sources == 0u || sources > APLIC_MAX_SOURCES) return false;
		struct dt_property indexes     = {0};
		bool               has_indexes = !msi && dt_node_property(node, "riscv,hart-indexes", &indexes);
		if (has_indexes && indexes.size != interrupts.size / 2u) return false;
		struct aplic_hart harts[APLIC_MAX_HARTS];
		size_t            hart_count    = 0u;
		uint32_t          largest_index = 0u;
		for (size_t entry = 0u; !msi && entry < interrupts.size / 8u; entry++) {
			uint64_t phandle_value;
			uint64_t cause;
			uint64_t index_value = entry;

			if (!dt_property_read_cells(&interrupts, entry * 2u, 1u, &phandle_value) ||
			    !dt_property_read_cells(&interrupts, entry * 2u + 1u, 1u, &cause) ||
			    (has_indexes && !dt_property_read_cells(&indexes, entry, 1u, &index_value)))
				return false;
			if (cause != 9u) continue;
			uint32_t phandle = (uint32_t)phandle_value;
			uint32_t index   = (uint32_t)index_value;
			uint64_t hart;
			if (index >= 16384u || !riscv64_interrupt_hart_for_phandle(phandle, &hart)) return false;
			for (size_t previous = 0u; previous < hart_count; previous++)
				if (harts[previous].hart_id == hart || harts[previous].index == index) return false;
			harts[hart_count++] = (struct aplic_hart){.hart_id = hart, .index = index};
			if (index > largest_index) largest_index = index;
		}
		if ((!msi &&
		     (hart_count == 0u || size < APLIC_IDC_BASE + ((uintptr_t)largest_index + 1u) * APLIC_IDC_STRIDE)) ||
		    size < APLIC_TARGET_BASE + (uintptr_t)sources * 4u || base > UINTPTR_MAX - size)
			return false;
		aplic.physical_base = base;
		aplic.size          = size;
		aplic.sources       = sources;
		aplic.hart_count    = hart_count;
		aplic.msi           = msi;
		memcpy(aplic.harts, harts, hart_count * sizeof(harts[0]));
	}
	return selected;
}

static bool aplic_map(uintptr_t physical, uintptr_t offset) {
	if (offset > aplic.size || aplic.size - offset < 4u || physical > UINTPTR_MAX - offset) return false;
	uintptr_t                 page = (physical + offset) & ~(uintptr_t)(APLIC_PAGE_SIZE - 1u);
	struct boot_address_space address_space;
	if (!boot_address_space_get(&address_space) || page > UINTPTR_MAX - address_space.direct_map_offset) return false;
	uintptr_t virt = page + address_space.direct_map_offset;
	if (hal_paging_query(hal_paging_kernel_space(), virt, NULL)) return true;
	uint64_t         flags = HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL;
	enum memory_type type =
		hal_paging_mapping_supported(flags, MEMORY_TYPE_DEVICE) ? MEMORY_TYPE_DEVICE : MEMORY_TYPE_NORMAL;
	return hal_paging_map(hal_paging_kernel_space(),
	                      &(const struct hal_paging_map_request){virt, page, APLIC_PAGE_SIZE, flags, type});
}

static uint32_t aplic_read(uintptr_t offset) {
	return *(volatile uint32_t*)(aplic.virtual_base + offset);
}

static void aplic_write(uintptr_t offset, uint32_t value) {
	*(volatile uint32_t*)(aplic.virtual_base + offset) = value;
}

static bool aplic_probe(void) {
	if (__atomic_load_n(&aplic.probed, __ATOMIC_ACQUIRE)) return aplic.ready;
	struct irq_state irq = irq_save_disable();
	while (__atomic_exchange_n(&aplic_init_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop");
	if (aplic.probed) goto done;
	struct boot_address_space address_space;
	if (!boot_address_space_get(&address_space) || !aplic_find() ||
	    aplic.physical_base > UINTPTR_MAX - address_space.direct_map_offset ||
	    !aplic_map(aplic.physical_base, APLIC_DOMAINCFG) ||
	    !aplic_map(aplic.physical_base, APLIC_SOURCECFG_BASE + (aplic.sources - 1u) * 4u) ||
	    !aplic_map(aplic.physical_base, APLIC_TARGET_BASE + (aplic.sources - 1u) * 4u) ||
	    !aplic_map(aplic.physical_base, APLIC_SETIENUM) || !aplic_map(aplic.physical_base, APLIC_CLRIENUM))
		goto done;
	if (aplic.msi && !aplic_map(aplic.physical_base, APLIC_SETIPNUM_LE)) goto done;
	for (size_t index = 0u; index < aplic.hart_count; index++) {
		uintptr_t idc = APLIC_IDC_BASE + (uintptr_t)aplic.harts[index].index * APLIC_IDC_STRIDE;
		if (!aplic_map(aplic.physical_base, idc + APLIC_IDC_CLAIMI)) goto done;
	}
	aplic.virtual_base = aplic.physical_base + address_space.direct_map_offset;
	for (uint32_t source = 0u; source <= aplic.sources; source += 32u)
		aplic_write(APLIC_CLRIE_BASE + (source / 32u) * 4u, UINT32_MAX);
	uint32_t config = aplic_read(APLIC_DOMAINCFG);
	config          = (config | APLIC_DOMAINCFG_IE) & ~APLIC_DOMAINCFG_DM;
	if (aplic.msi) config |= APLIC_DOMAINCFG_DM;
	aplic_write(APLIC_DOMAINCFG, config);
	if ((aplic_read(APLIC_DOMAINCFG) & (APLIC_DOMAINCFG_IE | APLIC_DOMAINCFG_DM)) !=
	    (config & (APLIC_DOMAINCFG_IE | APLIC_DOMAINCFG_DM)))
		goto done;
	for (size_t index = 0u; index < aplic.hart_count; index++) {
		uintptr_t idc = APLIC_IDC_BASE + (uintptr_t)aplic.harts[index].index * APLIC_IDC_STRIDE;
		aplic_write(idc + APLIC_IDC_ITHRESHOLD, 0u);
		aplic_write(idc + APLIC_IDC_IDELIVERY, 1u);
	}
	__atomic_store_n(&aplic.ready, true, __ATOMIC_RELEASE);
done:
	__atomic_store_n(&aplic.probed, true, __ATOMIC_RELEASE);
	__atomic_store_n(&aplic_init_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(irq);
	return aplic.ready;
}

static bool aplic_hart_for_cpu(const struct cpu* cpu, uint32_t* out_index) {
	if (cpu == NULL || !__atomic_load_n(&aplic.ready, __ATOMIC_ACQUIRE)) return false;
	for (size_t index = 0u; index < aplic.hart_count; index++) {
		if (aplic.harts[index].hart_id != cpu->arch_id) continue;
		if (out_index != NULL) *out_index = aplic.harts[index].index;
		return true;
	}
	return false;
}

bool riscv64_aplic_cpu_has_interface(const struct cpu* cpu) {
	return !aplic.msi && aplic_hart_for_cpu(cpu, NULL);
}

bool riscv64_aplic_source_info(const struct hal_interrupt_source* source, struct hal_interrupt_source_info* out) {
	if (source == NULL || out == NULL || source->domain != APLIC_DOMAIN_ID || !aplic_probe() || source->number == 0u ||
	    source->number > aplic.sources)
		return false;
	struct hal_interrupt_message_range message_range;
	if (aplic.msi && !riscv64_imsic_message_range_at(&message_range)) return false;
	*out = (struct hal_interrupt_source_info){
		.delivery     = aplic.msi ? message_range.delivery
		                          : (struct hal_interrupt_delivery_range){.domain = RISCV64_DELIVERY_DOMAIN_APLIC,
		                                                                  .base   = source->number,
		                                                                  .limit  = source->number + 1u},
		.target_kind  = HAL_INTERRUPT_TARGET_ROUTABLE,
		.fixed_target = NULL
    };
	return true;
}

bool riscv64_aplic_source_target_supported(const struct hal_interrupt_source* source, const struct cpu* target) {
	if (source == NULL || source->domain != APLIC_DOMAIN_ID || !__atomic_load_n(&aplic.ready, __ATOMIC_ACQUIRE) ||
	    source->number == 0u || source->number > aplic.sources || target == NULL)
		return false;
	if (!aplic.msi) return aplic_hart_for_cpu(target, NULL);
	return target->interrupts_ready && cpu_state_get(target) == CPU_STATE_ONLINE &&
	       riscv64_imsic_cpu_has_interface(target);
}

bool riscv64_aplic_source_domain_at(struct hal_interrupt_source_domain_info* out) {
	if (out == NULL || !aplic_probe()) return false;
	*out = (struct hal_interrupt_source_domain_info){
		.domain = APLIC_DOMAIN_ID, .first_source = 1u, .source_count = aplic.sources};
	return true;
}

bool riscv64_aplic_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                  struct hal_interrupt_source* out_source) {
	if (out_source == NULL || !aplic_probe() || controller_address != aplic.physical_base || local_source_id == 0u ||
	    local_source_id > aplic.sources)
		return false;
	*out_source = (struct hal_interrupt_source){.domain = APLIC_DOMAIN_ID, .number = local_source_id};
	return true;
}

bool riscv64_aplic_source_claimable(const struct hal_interrupt_source* source) {
	if (source == NULL || source->domain != APLIC_DOMAIN_ID || !aplic_probe() || source->number == 0u ||
	    source->number > aplic.sources)
		return false;
	uint32_t config_offset = APLIC_SOURCECFG_BASE + (source->number - 1u) * 4u;
	return (aplic_read(config_offset) & APLIC_SOURCECFG_DELEGATED) == 0u;
}

bool riscv64_aplic_source_init(struct hal_interrupt_source_state* state, const struct hal_interrupt_source* source,
                               const struct hal_interrupt_delivery* delivery) {
	struct hal_interrupt_source_info info;
	uint32_t                         hart_index;
	if (state == NULL || state->initialized || delivery == NULL || delivery->target == NULL ||
	    !riscv64_aplic_source_info(source, &info) || delivery->event.domain != info.delivery.domain ||
	    delivery->event.id < info.delivery.base || delivery->event.id >= info.delivery.limit ||
	    !riscv64_aplic_source_target_supported(source, delivery->target) ||
	    (aplic.msi ? (!delivery->target->interrupts_ready ||
	                  !riscv64_imsic_target(delivery->target, delivery->event.id, &hart_index))
	               : !aplic_hart_for_cpu(delivery->target, &hart_index)) ||
	    delivery->trigger == HAL_INTERRUPT_TRIGGER_FIRMWARE || delivery->polarity == HAL_INTERRUPT_POLARITY_FIRMWARE ||
	    delivery->trigger > HAL_INTERRUPT_TRIGGER_LEVEL || delivery->polarity > HAL_INTERRUPT_POLARITY_LOW)
		return false;
	uint32_t config_offset = APLIC_SOURCECFG_BASE + (source->number - 1u) * 4u;
	if ((aplic_read(config_offset) & APLIC_SOURCECFG_DELEGATED) != 0u) return false;
	aplic_write(APLIC_CLRIENUM, source->number);
	uint32_t mode;
	if (delivery->trigger == HAL_INTERRUPT_TRIGGER_EDGE)
		mode =
			delivery->polarity == HAL_INTERRUPT_POLARITY_HIGH ? APLIC_SOURCECFG_EDGE_RISE : APLIC_SOURCECFG_EDGE_FALL;
	else
		mode =
			delivery->polarity == HAL_INTERRUPT_POLARITY_HIGH ? APLIC_SOURCECFG_LEVEL_HIGH : APLIC_SOURCECFG_LEVEL_LOW;
	aplic_write(config_offset, mode);
	aplic_write(APLIC_TARGET_BASE + (source->number - 1u) * 4u,
	            (hart_index << 18u) | (aplic.msi ? delivery->event.id : 1u));
	if (aplic.msi && !riscv64_imsic_set_enabled(delivery->target, delivery->event.id, true)) {
		aplic_write(config_offset, 0u);
		return false;
	}
	*state = (struct hal_interrupt_source_state){.source      = *source,
	                                             .target      = delivery->target,
	                                             .route       = aplic.msi ? delivery->event.id : hart_index,
	                                             .initialized = true,
	                                             .masked      = true};
	return true;
}

bool riscv64_aplic_source_mask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || state->source.domain != APLIC_DOMAIN_ID || !aplic.ready) return false;
	aplic_write(APLIC_CLRIENUM, state->source.number);
	state->masked = true;
	return true;
}

bool riscv64_aplic_source_unmask(struct hal_interrupt_source_state* state) {
	if (state == NULL || !state->initialized || state->source.domain != APLIC_DOMAIN_ID || !aplic.ready) return false;
	aplic_write(APLIC_SETIENUM, state->source.number);
	state->masked = false;
	if (!aplic.msi) {
		if (state->target == cpu_current()) riscv64_sync_external_interrupt_local();
		else hal_cpu_kick(state->target);
	}
	return true;
}

bool riscv64_aplic_source_deinit(struct hal_interrupt_source_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->source.domain != APLIC_DOMAIN_ID || !riscv64_aplic_source_mask(state) ||
	    (aplic.msi && !riscv64_imsic_set_enabled(state->target, state->route, false)))
		return false;
	aplic_write(APLIC_SOURCECFG_BASE + (state->source.number - 1u) * 4u, 0u);
	state->initialized = false;
	return true;
}

bool riscv64_aplic_handle_external_irq(void) {
	if (aplic.msi) return false;
	uint32_t hart_index;
	if (!aplic_hart_for_cpu(cpu_current(), &hart_index)) return false;
	uint32_t claim  = aplic_read(APLIC_IDC_BASE + (uintptr_t)hart_index * APLIC_IDC_STRIDE + APLIC_IDC_CLAIMI);
	uint32_t source = claim >> 16u;
	if (source == 0u) return false;
	if (source <= aplic.sources &&
	    !interrupt_handle_event((struct hal_interrupt_event){.domain = RISCV64_DELIVERY_DOMAIN_APLIC, .id = source}))
		aplic_write(APLIC_CLRIENUM, source);
	return true;
}

bool riscv64_aplic_handle_message_id(uint32_t id) {
	if (!aplic.ready || !aplic.msi || id == 0u) return false;
	uint32_t hart_index;
	if (!riscv64_imsic_target(cpu_current(), id, &hart_index)) return false;
	for (uint32_t source = 1u; source <= aplic.sources; source++) {
		uint32_t config = aplic_read(APLIC_SOURCECFG_BASE + (source - 1u) * 4u);
		if (config == 0u || (config & APLIC_SOURCECFG_DELEGATED) != 0u) continue;
		uint32_t target = aplic_read(APLIC_TARGET_BASE + (source - 1u) * 4u);
		if ((target >> 18u) != hart_index || (target & 0x7ffu) != id) continue;
		aplic_write(APLIC_CLRIENUM, source);
		if (config == APLIC_SOURCECFG_LEVEL_HIGH || config == APLIC_SOURCECFG_LEVEL_LOW)
			aplic_write(APLIC_SETIPNUM_LE, source);
		return true;
	}
	return false;
}
