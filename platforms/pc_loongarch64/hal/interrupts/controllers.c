#include "controllers.h"

#include <core/cpu.h>
#include <hal/paging.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../../iommu_acpi.h"
#include "../../../iommu_fdt.h"
#include "avec.h"
#include "controller.h"
#include "eiointc.h"
#include "fixed.h"
#include "liointc.h"
#include "pch_msi.h"
#include "redirect.h"

#define LOONGARCH64_PAGE_SIZE 0x1000u

struct acpi_madt_lio_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint64_t address;
	uint16_t size;
	uint8_t  cascade[2];
	uint32_t cascade_map[2];
} __attribute__((packed));

struct acpi_madt_ht_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint64_t address;
	uint16_t size;
	uint8_t  cascade[8];
} __attribute__((packed));

struct acpi_madt_eio_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint8_t  cascade;
	uint8_t  node;
	uint64_t node_map;
} __attribute__((packed));

struct acpi_madt_msi_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint64_t address;
	uint32_t first;
	uint32_t count;
} __attribute__((packed));

struct acpi_madt_bio_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint64_t address;
	uint16_t size;
	uint16_t id;
	uint16_t gsi_base;
} __attribute__((packed));

struct acpi_madt_lpc_pic {
	uint8_t  type;
	uint8_t  length;
	uint8_t  version;
	uint64_t address;
	uint16_t size;
	uint8_t  cascade;
} __attribute__((packed));

struct fixed_controller fixed_controllers[LOONGARCH64_MAX_FIXED_CONTROLLERS];
size_t                  fixed_controller_count;
struct eiointc_state    eiointc;
struct htvec_state      htvec;
struct pch_msi_state    pch_msi;
uint8_t                 lio_cascades[2];
uint32_t                lio_cascade_maps[2];
bool                    controllers_discovered;
const struct cpu*       loongarch64_fixed_target;

static uint8_t controller_lock;

uint32_t loongarch64_mmio_read32(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint32_t*)(base + offset);
}

uint8_t loongarch64_mmio_read8(volatile uint8_t* base, uint32_t offset) {
	return *(volatile uint8_t*)(base + offset);
}

void loongarch64_mmio_write32(volatile uint8_t* base, uint32_t offset, uint32_t value) {
	*(volatile uint32_t*)(base + offset) = value;
}

void loongarch64_mmio_write8(volatile uint8_t* base, uint32_t offset, uint8_t value) {
	*(volatile uint8_t*)(base + offset) = value;
}

struct irq_state loongarch64_controllers_lock(void) {
	struct irq_state state = irq_save_disable();
	while (__atomic_exchange_n(&controller_lock, 1u, __ATOMIC_ACQUIRE) != 0u) __asm__ volatile("nop");
	return state;
}

void loongarch64_controllers_unlock(struct irq_state state) {
	__atomic_store_n(&controller_lock, 0u, __ATOMIC_RELEASE);
	irq_restore(state);
}

bool loongarch64_map_mmio(uintptr_t physical, uintptr_t size, volatile uint8_t** out) {
	struct kernel_boot_address_space address_space;
	if (out == NULL || size == 0u || physical > UINTPTR_MAX - size || !kernel_boot_address_space_get(&address_space))
		return false;
	uintptr_t first = physical & ~(uintptr_t)(LOONGARCH64_PAGE_SIZE - 1u);
	uintptr_t last  = (physical + size - 1u) & ~(uintptr_t)(LOONGARCH64_PAGE_SIZE - 1u);
	for (uintptr_t page = first;; page += LOONGARCH64_PAGE_SIZE) {
		if (page > UINTPTR_MAX - address_space.direct_map_offset) return false;
		uintptr_t virtual_page = page + address_space.direct_map_offset;
		if (!hal_paging_query(hal_paging_kernel_space(), virtual_page, NULL) &&
		    !hal_paging_map(hal_paging_kernel_space(),
		                    &(const struct hal_paging_map_request){virtual_page,
		                                                           page,
		                                                           LOONGARCH64_PAGE_SIZE,
		                                                           HAL_PAGE_READ | HAL_PAGE_WRITE | HAL_PAGE_GLOBAL,
		                                                           MEMORY_TYPE_DEVICE}))
			return false;
		if (page == last) break;
	}
	if (physical > UINTPTR_MAX - address_space.direct_map_offset) return false;
	*out = (volatile uint8_t*)(physical + address_space.direct_map_offset);
	return true;
}

static bool add_fixed(enum fixed_kind kind, uint32_t domain, uintptr_t address, uintptr_t size, uint32_t source_count,
                      uint32_t vector_base, uint32_t cascade) {
	if (address == 0u || size == 0u || source_count == 0u ||
	    fixed_controller_count == LOONGARCH64_MAX_FIXED_CONTROLLERS)
		return false;
	for (size_t index = 0u; index < fixed_controller_count; index++)
		if (fixed_controllers[index].domain == domain) return false;
	fixed_controllers[fixed_controller_count++] = (struct fixed_controller){.kind          = kind,
	                                                                        .domain        = domain,
	                                                                        .source_count  = source_count,
	                                                                        .vector_base   = vector_base,
	                                                                        .cascade       = cascade,
	                                                                        .physical_base = address,
	                                                                        .size          = size};
	return true;
}

struct fixed_controller* loongarch64_fixed_by_domain(uint32_t domain) {
	for (size_t index = 0u; index < fixed_controller_count; index++)
		if (fixed_controllers[index].domain == domain) return &fixed_controllers[index];
	return NULL;
}

static bool fdt_u32_property(const char* compatible, const char* name, uint32_t* out) {
	const uint8_t* data;
	size_t         size;
	if (out == NULL || !iommu_fdt_controller_property(compatible, 0u, name, &data, &size) || size != 4u) return false;
	*out = iommu_fdt_u32(data);
	return true;
}

static bool discover_fdt(void) {
	uintptr_t   eio_address    = 0u;
	const char* eio_compatible = "loongson,ls2k2000-eiointc";
	if (iommu_fdt_controllers(eio_compatible, 0u, &eio_address) == 0u) {
		eio_compatible = "loongson,ls2k0500-eiointc";
		if (iommu_fdt_controllers(eio_compatible, 0u, &eio_address) == 0u) return false;
		eiointc.vector_count = 128u;
	}
	else eiointc.vector_count = EIOINTC_VECTOR_COUNT;
	uint32_t cascade;
	if (!fdt_u32_property(eio_compatible, "interrupts", &cascade) || cascade < LOONGARCH64_CPU_HWI_BASE ||
	    cascade >= LOONGARCH64_CPU_HWI_LIMIT)
		return false;
	eiointc.cascade   = cascade;
	eiointc.node_map  = UINT64_MAX;
	eiointc.described = true;

	uintptr_t pch_address;
	uintptr_t pch_size;
	uint32_t  vector_base;
	if (iommu_fdt_controllers_region("loongson,pch-pic-1.0", 0u, &pch_address, &pch_size) == 1u &&
	    fdt_u32_property("loongson,pch-pic-1.0", "loongson,pic-base-vec", &vector_base))
		(void)add_fixed(FIXED_PCH_PIC,
		                LOONGARCH64_DOMAIN_PCH_PIC_BASE,
		                pch_address,
		                pch_size,
		                PCH_PIC_SOURCE_COUNT,
		                vector_base,
		                UINT32_MAX);

	uintptr_t msi_address;
	uint32_t  msi_first;
	uint32_t  msi_count;
	if (iommu_fdt_controllers("loongson,pch-msi-1.0", 0u, &msi_address) == 1u &&
	    fdt_u32_property("loongson,pch-msi-1.0", "loongson,msi-base-vec", &msi_first) &&
	    fdt_u32_property("loongson,pch-msi-1.0", "loongson,msi-num-vecs", &msi_count) && msi_count != 0u &&
	    msi_first < eiointc.vector_count && msi_count <= eiointc.vector_count - msi_first) {
		pch_msi.address   = msi_address;
		pch_msi.first     = msi_first;
		pch_msi.count     = msi_count;
		pch_msi.described = true;
	}
	return true;
}

static bool discover_acpi(void) {
	const struct iommu_acpi_header* madt = iommu_acpi_table("APIC");
	if (madt == NULL || madt->length < sizeof(*madt) + 8u) return false;
	const uint8_t* entry     = (const uint8_t*)madt + sizeof(*madt) + 8u;
	const uint8_t* end       = (const uint8_t*)madt + madt->length;
	uint32_t       pch_index = 0u;
	while ((size_t)(end - entry) >= 2u) {
		uint8_t type   = entry[0];
		uint8_t length = entry[1];
		if (length < 2u || length > (size_t)(end - entry)) return false;
		if (type == 18u && length >= sizeof(struct acpi_madt_lio_pic)) {
			const struct acpi_madt_lio_pic* lio = (const struct acpi_madt_lio_pic*)entry;
			if (lio->version == 1u && lio->address <= UINTPTR_MAX) {
				memcpy(lio_cascades, lio->cascade, sizeof(lio_cascades));
				memcpy(lio_cascade_maps, lio->cascade_map, sizeof(lio_cascade_maps));
				(void)add_fixed(FIXED_LIOINTC,
				                LOONGARCH64_DOMAIN_LIOINTC,
				                (uintptr_t)lio->address,
				                lio->size,
				                LIOINTC_SOURCE_COUNT,
				                0u,
				                lio->cascade[0]);
			}
		}
		else if (type == 19u && length >= sizeof(struct acpi_madt_ht_pic)) {
			const struct acpi_madt_ht_pic* ht = (const struct acpi_madt_ht_pic*)entry;
			if (ht->version == 1u && ht->address <= UINTPTR_MAX && ht->size != 0u) {
				htvec.physical_base = (uintptr_t)ht->address;
				htvec.size          = ht->size;
				memcpy(htvec.cascades, ht->cascade, sizeof(htvec.cascades));
				htvec.described = true;
			}
		}
		else if (type == 20u && length >= sizeof(struct acpi_madt_eio_pic)) {
			const struct acpi_madt_eio_pic* eio = (const struct acpi_madt_eio_pic*)entry;
			if (eio->version == 1u && eio->cascade >= LOONGARCH64_CPU_HWI_BASE &&
			    eio->cascade < LOONGARCH64_CPU_HWI_LIMIT)
				eiointc = (struct eiointc_state){.cascade      = eio->cascade,
				                                 .vector_count = EIOINTC_VECTOR_COUNT,
				                                 .node         = eio->node,
				                                 .node_map     = eio->node_map,
				                                 .described    = true};
		}
		else if (type == 21u && length >= sizeof(struct acpi_madt_msi_pic)) {
			const struct acpi_madt_msi_pic* msi = (const struct acpi_madt_msi_pic*)entry;
			if (msi->version == 1u && msi->address != 0u && msi->count != 0u && msi->first < EIOINTC_VECTOR_COUNT &&
			    msi->count <= EIOINTC_VECTOR_COUNT - msi->first)
				pch_msi = (struct pch_msi_state){
					.address = msi->address, .first = msi->first, .count = msi->count, .described = true};
		}
		else if (type == 22u && length >= sizeof(struct acpi_madt_bio_pic) && pch_index < LOONGARCH64_MAX_PCH_PICS) {
			const struct acpi_madt_bio_pic* pch = (const struct acpi_madt_bio_pic*)entry;
			if (pch->version == 1u && pch->address <= UINTPTR_MAX &&
			    add_fixed(FIXED_PCH_PIC,
			              LOONGARCH64_DOMAIN_PCH_PIC_BASE + pch_index,
			              (uintptr_t)pch->address,
			              pch->size,
			              PCH_PIC_SOURCE_COUNT,
			              0u,
			              UINT32_MAX))
				pch_index++;
		}
		else if (type == 23u && length >= sizeof(struct acpi_madt_lpc_pic)) {
			const struct acpi_madt_lpc_pic* lpc = (const struct acpi_madt_lpc_pic*)entry;
			if (lpc->version == 1u && lpc->address <= UINTPTR_MAX)
				(void)add_fixed(FIXED_PCH_LPC,
				                LOONGARCH64_DOMAIN_PCH_LPC,
				                (uintptr_t)lpc->address,
				                lpc->size,
				                LPC_SOURCE_COUNT,
				                0u,
				                lpc->cascade);
		}
		entry += length;
	}
	return entry == end && (eiointc.described || htvec.described || fixed_controller_count != 0u);
}

bool loongarch64_interrupt_controllers_discover(void) {
	if (controllers_discovered) return true;
	memset(fixed_controllers, 0, sizeof(fixed_controllers));
	fixed_controller_count = 0u;
	memset(&eiointc, 0, sizeof(eiointc));
	memset(&htvec, 0, sizeof(htvec));
	memset(&pch_msi, 0, sizeof(pch_msi));
	memset(lio_cascades, 0, sizeof(lio_cascades));
	memset(lio_cascade_maps, 0, sizeof(lio_cascade_maps));
	if (!discover_fdt()) (void)discover_acpi();
	loongarch64_avec_discover();
	loongarch64_fixed_target = cpu_bsp();
	if (loongarch64_fixed_target == NULL) return false;
	controllers_discovered = true;
	return true;
}

bool loongarch64_interrupt_controllers_init_local(const struct cpu* cpu) {
	if (!controllers_discovered || cpu == NULL) return false;
	if (cpu->role == CPU_ROLE_BSP) loongarch64_fixed_target = cpu;
	return loongarch64_eiointc_init_local(cpu) && loongarch64_avec_init_local(cpu);
}

size_t loongarch64_interrupt_source_domain_count(void) {
	return controllers_discovered ? loongarch64_fixed_source_domain_count() : 0u;
}

bool loongarch64_interrupt_source_domain_at(size_t index, struct hal_interrupt_source_domain_info* out_domain) {
	return controllers_discovered && loongarch64_fixed_source_domain_at(index, out_domain);
}

bool loongarch64_interrupt_source_resolve(uint64_t controller_address, uint32_t local_source_id,
                                          struct hal_interrupt_source* out_source) {
	return controllers_discovered && loongarch64_fixed_source_resolve(controller_address, local_source_id, out_source);
}

bool loongarch64_interrupt_source_configuration_supported(const struct hal_interrupt_source* source,
                                                          enum hal_interrupt_trigger         trigger,
                                                          enum hal_interrupt_polarity        polarity) {
	return controllers_discovered && loongarch64_fixed_source_configuration_supported(source, trigger, polarity);
}

bool loongarch64_interrupt_source_info(const struct hal_interrupt_source* source,
                                       struct hal_interrupt_source_info*  out_info) {
	return controllers_discovered && loongarch64_fixed_source_info(source, out_info);
}

bool loongarch64_interrupt_source_target_supported(const struct hal_interrupt_source* source,
                                                   const struct cpu*                  target) {
	return controllers_discovered && loongarch64_fixed_source_target_supported(source, target);
}

bool loongarch64_interrupt_source_init(struct hal_interrupt_source_state*   state,
                                       const struct hal_interrupt_source*   source,
                                       const struct hal_interrupt_delivery* delivery) {
	return controllers_discovered && loongarch64_fixed_source_init(state, source, delivery);
}

bool loongarch64_interrupt_source_mask(struct hal_interrupt_source_state* state) {
	return controllers_discovered && loongarch64_fixed_source_mask(state);
}

bool loongarch64_interrupt_source_unmask(struct hal_interrupt_source_state* state) {
	return controllers_discovered && loongarch64_fixed_source_unmask(state);
}

bool loongarch64_interrupt_source_deinit(struct hal_interrupt_source_state* state) {
	return controllers_discovered && loongarch64_fixed_source_deinit(state);
}

size_t loongarch64_interrupt_message_range_count(void) {
	if (!controllers_discovered) return 0u;
	return loongarch64_avec_available() ? 1u : loongarch64_pch_msi_range_count();
}

bool loongarch64_interrupt_message_range_at(size_t index, struct hal_interrupt_message_range* out_range) {
	if (!controllers_discovered) return false;
	return loongarch64_avec_available() ? index == 0u && loongarch64_avec_range(out_range)
	                                    : loongarch64_pch_msi_range_at(index, out_range);
}

bool loongarch64_interrupt_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                                    const struct cpu* target) {
	if (!controllers_discovered) return false;
	if (domain == LOONGARCH64_MESSAGE_DOMAIN_AVEC) return loongarch64_avec_target_supported(domain, source, target);
	if (domain == LOONGARCH64_MESSAGE_DOMAIN_REDIRECT)
		return loongarch64_redirect_target_supported(domain, source, target);
	if (loongarch64_avec_available()) return false;
	return loongarch64_pch_msi_target_supported(domain, source, target);
}

bool loongarch64_interrupt_message_init(struct hal_interrupt_message_state*         state,
                                        const struct hal_interrupt_message_request* request,
                                        struct hal_interrupt_message*               out_message) {
	if (!controllers_discovered || request == NULL) return false;
	if (request->domain == LOONGARCH64_MESSAGE_DOMAIN_AVEC) return loongarch64_avec_init(state, request, out_message);
	if (request->domain == LOONGARCH64_MESSAGE_DOMAIN_REDIRECT)
		return loongarch64_redirect_init(state, request, out_message);
	if (loongarch64_avec_available()) return false;
	return loongarch64_pch_msi_init(state, request, out_message);
}

bool loongarch64_interrupt_message_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (!controllers_discovered) return false;
	if (state->domain == LOONGARCH64_MESSAGE_DOMAIN_AVEC) return loongarch64_avec_deinit(state);
	if (state->domain == LOONGARCH64_MESSAGE_DOMAIN_REDIRECT) return loongarch64_redirect_deinit(state);
	return loongarch64_pch_msi_deinit(state);
}

bool loongarch64_interrupt_controllers_handle(uint64_t pending) {
	if ((pending & (1ull << 14u)) != 0u && loongarch64_avec_handle()) return true;
	if (eiointc.described && (pending & (1ull << eiointc.cascade)) != 0u && loongarch64_eiointc_handle()) return true;
	return loongarch64_liointc_handle(pending);
}

bool loongarch64_interrupt_controllers_has_avec(void) {
	return controllers_discovered && loongarch64_avec_available();
}
