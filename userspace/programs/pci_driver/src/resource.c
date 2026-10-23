#include "resource.h"

#include <stddef.h>

#define PCI_COMMAND_OFFSET 0x04u
#define PCI_STATUS_OFFSET 0x06u
#define PCI_BAR_OFFSET 0x10u
#define PCI_CAPABILITY_POINTER_OFFSET 0x34u
#define PCI_CARDBUS_CAPABILITY_POINTER_OFFSET 0x14u
#define PCI_INTERRUPT_LINE_OFFSET 0x3cu
#define PCI_INTERRUPT_PIN_OFFSET 0x3du

#define PCI_COMMAND_IO_ENABLE (1u << 0u)
#define PCI_COMMAND_MEMORY_ENABLE (1u << 1u)
#define PCI_STATUS_CAPABILITIES (1u << 4u)

#define PCI_HEADER_LAYOUT_MASK 0x7fu
#define PCI_HEADER_ENDPOINT 0u
#define PCI_HEADER_BRIDGE 1u
#define PCI_HEADER_CARDBUS 2u

#define PCI_BAR_IO_INDICATOR (1u << 0u)
#define PCI_BAR_MEMORY_TYPE_MASK (3u << 1u)
#define PCI_BAR_MEMORY_TYPE_32BIT (0u << 1u)
#define PCI_BAR_MEMORY_TYPE_64BIT (2u << 1u)
#define PCI_BAR_MEMORY_PREFETCHABLE (1u << 3u)
#define PCI_BAR_IO_ADDRESS_MASK UINT32_C(0xfffffffc)
#define PCI_BAR_MEMORY_ADDRESS_MASK UINT32_C(0xfffffff0)

#define PCI_ROM_ENDPOINT_OFFSET 0x30u
#define PCI_ROM_BRIDGE_OFFSET 0x38u
#define PCI_ROM_ENABLE (1u << 0u)
#define PCI_ROM_ADDRESS_MASK UINT32_C(0xfffff800)

#define PCI_CAPABILITY_MINIMUM_OFFSET 0x40u
#define PCI_CAPABILITY_ID_MSI 0x05u
#define PCI_CAPABILITY_ID_MSIX 0x11u
#define PCI_CAPABILITY_MAXIMUM_COUNT 48u
#define PCI_MSI_CONTROL_ENABLE (1u << 0u)
#define PCI_MSI_CONTROL_CAPABLE_SHIFT 1u
#define PCI_MSI_CONTROL_CAPABLE_MASK 7u
#define PCI_MSI_CONTROL_64BIT (1u << 7u)
#define PCI_MSI_CONTROL_MASKING (1u << 8u)
#define PCI_MSIX_CONTROL_TABLE_SIZE_MASK 0x07ffu
#define PCI_MSIX_CONTROL_FUNCTION_MASK (1u << 14u)
#define PCI_MSIX_CONTROL_ENABLE (1u << 15u)
#define PCI_MSIX_BAR_MASK 7u
#define PCI_MSIX_OFFSET_MASK UINT32_C(0xfffffff8)

#define PCI_BRIDGE_PRIMARY_BUS_OFFSET 0x18u
#define PCI_BRIDGE_SECONDARY_BUS_OFFSET 0x19u
#define PCI_BRIDGE_SUBORDINATE_BUS_OFFSET 0x1au
#define PCI_BRIDGE_IO_BASE_OFFSET 0x1cu
#define PCI_BRIDGE_IO_LIMIT_OFFSET 0x1du
#define PCI_BRIDGE_MEMORY_BASE_OFFSET 0x20u
#define PCI_BRIDGE_MEMORY_LIMIT_OFFSET 0x22u
#define PCI_BRIDGE_PREFETCHABLE_BASE_OFFSET 0x24u
#define PCI_BRIDGE_PREFETCHABLE_LIMIT_OFFSET 0x26u
#define PCI_BRIDGE_PREFETCHABLE_BASE_UPPER_OFFSET 0x28u
#define PCI_BRIDGE_PREFETCHABLE_LIMIT_UPPER_OFFSET 0x2cu
#define PCI_BRIDGE_IO_BASE_UPPER_OFFSET 0x30u
#define PCI_BRIDGE_IO_LIMIT_UPPER_OFFSET 0x32u
#define PCI_BRIDGE_RANGE_TYPE_MASK 0x0fu
#define PCI_BRIDGE_IO_RANGE_32BIT 0x01u
#define PCI_BRIDGE_MEMORY_RANGE_64BIT 0x01u

static uint64_t range_size(uint64_t base, uint64_t limit) {
	return base <= limit ? limit - base + 1u : 0u;
}

static bool bar_probe32(struct pci_config* config, struct pci_address address, uint16_t offset, uint32_t original,
                        uint32_t address_mask, uint32_t* out_mask) {
	uint32_t mask;
	bool     success;

	if (!pci_config_write32(config, address, offset, UINT32_MAX)) return false;
	success = pci_config_read32(config, address, offset, &mask);
	if (!pci_config_write32(config, address, offset, original)) return false;
	if (!success) return false;
	*out_mask = mask & address_mask;
	return true;
}

static bool bar_probe64(struct pci_config* config, struct pci_address address, uint16_t offset, uint32_t original_low,
                        uint32_t original_high, uint64_t* out_mask) {
	uint32_t mask_low;
	uint32_t mask_high;
	bool     success = false;

	if (!pci_config_write32(config, address, offset, UINT32_MAX)) return false;
	if (!pci_config_write32(config, address, (uint16_t)(offset + 4u), UINT32_MAX)) goto restore_low;
	if (!pci_config_read32(config, address, offset, &mask_low) ||
	    !pci_config_read32(config, address, (uint16_t)(offset + 4u), &mask_high))
		goto restore_both;
	success   = true;
	*out_mask = ((uint64_t)mask_high << 32u) | (mask_low & PCI_BAR_MEMORY_ADDRESS_MASK);

restore_both:
	if (!pci_config_write32(config, address, (uint16_t)(offset + 4u), original_high)) success = false;
restore_low:
	if (!pci_config_write32(config, address, offset, original_low)) success = false;
	return success;
}

static bool detect_bars(struct pci_config* config, struct pci_address address, uint8_t header_layout,
                        struct pci_resources* resources) {
	uint8_t implemented_count = header_layout == PCI_HEADER_ENDPOINT ? 6u
	                            : header_layout == PCI_HEADER_BRIDGE ? 2u
	                                                                 : 0u;

	for (uint8_t index = 0u; index < implemented_count; index++) {
		struct pci_bar_resource* bar    = &resources->bars[resources->bar_count];
		uint16_t                 offset = (uint16_t)(PCI_BAR_OFFSET + index * 4u);
		uint32_t                 original_low;
		uint32_t                 mask_low;

		if (!pci_config_read32(config, address, offset, &original_low)) return false;
		bar->index = index;
		if ((original_low & PCI_BAR_IO_INDICATOR) != 0u) {
			if (!bar_probe32(config, address, offset, original_low, PCI_BAR_IO_ADDRESS_MASK, &mask_low)) return false;
			if (mask_low == 0u) continue;
			bar->kind    = PCI_BAR_IO;
			bar->address = original_low & PCI_BAR_IO_ADDRESS_MASK;
			bar->size    = (uint32_t)(~mask_low + 1u);
		}
		else if ((original_low & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_TYPE_64BIT) {
			uint32_t original_high;
			uint64_t mask;

			if (index + 1u >= implemented_count ||
			    !pci_config_read32(config, address, (uint16_t)(offset + 4u), &original_high) ||
			    !bar_probe64(config, address, offset, original_low, original_high, &mask))
				return false;
			if (mask != 0u) {
				bar->kind         = PCI_BAR_MEMORY64;
				bar->address      = ((uint64_t)original_high << 32u) | (original_low & PCI_BAR_MEMORY_ADDRESS_MASK);
				bar->size         = ~mask + 1u;
				bar->prefetchable = (original_low & PCI_BAR_MEMORY_PREFETCHABLE) != 0u;
				resources->bar_count++;
			}
			index++;
			continue;
		}
		else if ((original_low & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_TYPE_32BIT) {
			if (!bar_probe32(config, address, offset, original_low, PCI_BAR_MEMORY_ADDRESS_MASK, &mask_low))
				return false;
			if (mask_low == 0u) continue;
			bar->kind         = PCI_BAR_MEMORY32;
			bar->address      = original_low & PCI_BAR_MEMORY_ADDRESS_MASK;
			bar->size         = (uint32_t)(~mask_low + 1u);
			bar->prefetchable = (original_low & PCI_BAR_MEMORY_PREFETCHABLE) != 0u;
		}
		else {
			bar->kind = PCI_BAR_RESERVED;
			resources->bar_count++;
			continue;
		}
		if (bar->size != 0u) resources->bar_count++;
	}
	return true;
}

static bool detect_rom(struct pci_config* config, struct pci_address address, uint8_t header_layout,
                       struct pci_rom_resource* rom) {
	uint16_t offset;
	uint32_t original;
	uint32_t mask;

	if (header_layout == PCI_HEADER_ENDPOINT) offset = PCI_ROM_ENDPOINT_OFFSET;
	else if (header_layout == PCI_HEADER_BRIDGE) offset = PCI_ROM_BRIDGE_OFFSET;
	else return true;
	if (!pci_config_read32(config, address, offset, &original) ||
	    !bar_probe32(config, address, offset, original, PCI_ROM_ADDRESS_MASK, &mask))
		return false;
	if (mask == 0u) return true;
	rom->present = true;
	rom->enabled = (original & PCI_ROM_ENABLE) != 0u;
	rom->address = original & PCI_ROM_ADDRESS_MASK;
	rom->size    = (uint32_t)(~mask + 1u);
	return true;
}

static bool detect_msi(struct pci_config* config, struct pci_address address, uint8_t offset,
                       struct pci_msi_resource* msi) {
	uint16_t control;

	if (!pci_config_read16(config, address, (uint16_t)(offset + 2u), &control)) return false;
	msi->present           = true;
	msi->capability_offset = offset;
	msi->vector_count  = (uint16_t)(1u << ((control >> PCI_MSI_CONTROL_CAPABLE_SHIFT) & PCI_MSI_CONTROL_CAPABLE_MASK));
	msi->enabled       = (control & PCI_MSI_CONTROL_ENABLE) != 0u;
	msi->address_64bit = (control & PCI_MSI_CONTROL_64BIT) != 0u;
	msi->per_vector_masking = (control & PCI_MSI_CONTROL_MASKING) != 0u;
	return true;
}

static bool detect_msix(struct pci_config* config, struct pci_address address, uint8_t offset,
                        struct pci_msix_resource* msix) {
	uint16_t control;
	uint32_t table;
	uint32_t pba;

	if (!pci_config_read16(config, address, (uint16_t)(offset + 2u), &control) ||
	    !pci_config_read32(config, address, (uint16_t)(offset + 4u), &table) ||
	    !pci_config_read32(config, address, (uint16_t)(offset + 8u), &pba))
		return false;
	msix->present           = true;
	msix->capability_offset = offset;
	msix->vector_count      = (uint16_t)((control & PCI_MSIX_CONTROL_TABLE_SIZE_MASK) + 1u);
	msix->table_bar         = (uint8_t)(table & PCI_MSIX_BAR_MASK);
	msix->table_offset      = table & PCI_MSIX_OFFSET_MASK;
	msix->pba_bar           = (uint8_t)(pba & PCI_MSIX_BAR_MASK);
	msix->pba_offset        = pba & PCI_MSIX_OFFSET_MASK;
	msix->enabled           = (control & PCI_MSIX_CONTROL_ENABLE) != 0u;
	msix->function_masked   = (control & PCI_MSIX_CONTROL_FUNCTION_MASK) != 0u;
	return true;
}

static bool detect_capabilities(struct pci_config* config, struct pci_address address, uint8_t header_layout,
                                struct pci_resources* resources) {
	uint64_t visited = 0u;
	uint8_t  pointer;

	if ((resources->status & PCI_STATUS_CAPABILITIES) == 0u) return true;
	if (!pci_config_read8(config,
	                      address,
	                      header_layout == PCI_HEADER_CARDBUS ? PCI_CARDBUS_CAPABILITY_POINTER_OFFSET
	                                                          : PCI_CAPABILITY_POINTER_OFFSET,
	                      &pointer))
		return false;
	for (uint8_t count = 0u; count < PCI_CAPABILITY_MAXIMUM_COUNT && pointer != 0u; count++) {
		uint8_t  capability_id;
		uint8_t  next;
		uint8_t  aligned = pointer & (uint8_t)0xfcu;
		uint64_t bit;

		if (aligned < PCI_CAPABILITY_MINIMUM_OFFSET) return false;
		bit = (uint64_t)1u << (aligned / 4u);
		if ((visited & bit) != 0u) return false;
		visited |= bit;
		if (!pci_config_read8(config, address, aligned, &capability_id) ||
		    !pci_config_read8(config, address, (uint16_t)(aligned + 1u), &next))
			return false;
		if (capability_id == PCI_CAPABILITY_ID_MSI && !resources->msi.present) {
			if (!detect_msi(config, address, aligned, &resources->msi)) return false;
		}
		else if (capability_id == PCI_CAPABILITY_ID_MSIX && !resources->msix.present) {
			if (!detect_msix(config, address, aligned, &resources->msix)) return false;
		}
		pointer = next;
	}
	return pointer == 0u;
}

static bool detect_bridge(struct pci_config* config, struct pci_address address, struct pci_bridge_resources* bridge) {
	uint8_t  io_base;
	uint8_t  io_limit;
	uint16_t io_base_upper;
	uint16_t io_limit_upper;
	uint16_t memory_base;
	uint16_t memory_limit;
	uint16_t prefetchable_base;
	uint16_t prefetchable_limit;
	uint32_t prefetchable_base_upper;
	uint32_t prefetchable_limit_upper;
	uint64_t base;
	uint64_t limit;

	bridge->present = true;
	if (!pci_config_read8(config, address, PCI_BRIDGE_PRIMARY_BUS_OFFSET, &bridge->primary_bus) ||
	    !pci_config_read8(config, address, PCI_BRIDGE_SECONDARY_BUS_OFFSET, &bridge->secondary_bus) ||
	    !pci_config_read8(config, address, PCI_BRIDGE_SUBORDINATE_BUS_OFFSET, &bridge->subordinate_bus) ||
	    !pci_config_read8(config, address, PCI_BRIDGE_IO_BASE_OFFSET, &io_base) ||
	    !pci_config_read8(config, address, PCI_BRIDGE_IO_LIMIT_OFFSET, &io_limit) ||
	    !pci_config_read16(config, address, PCI_BRIDGE_MEMORY_BASE_OFFSET, &memory_base) ||
	    !pci_config_read16(config, address, PCI_BRIDGE_MEMORY_LIMIT_OFFSET, &memory_limit) ||
	    !pci_config_read16(config, address, PCI_BRIDGE_PREFETCHABLE_BASE_OFFSET, &prefetchable_base) ||
	    !pci_config_read16(config, address, PCI_BRIDGE_PREFETCHABLE_LIMIT_OFFSET, &prefetchable_limit))
		return false;

	io_base_upper  = 0u;
	io_limit_upper = 0u;
	if ((io_base & PCI_BRIDGE_RANGE_TYPE_MASK) == PCI_BRIDGE_IO_RANGE_32BIT ||
	    (io_limit & PCI_BRIDGE_RANGE_TYPE_MASK) == PCI_BRIDGE_IO_RANGE_32BIT) {
		if (!pci_config_read16(config, address, PCI_BRIDGE_IO_BASE_UPPER_OFFSET, &io_base_upper) ||
		    !pci_config_read16(config, address, PCI_BRIDGE_IO_LIMIT_UPPER_OFFSET, &io_limit_upper))
			return false;
	}
	base               = ((uint64_t)io_base_upper << 16u) | ((uint64_t)(io_base & 0xf0u) << 8u);
	limit              = ((uint64_t)io_limit_upper << 16u) | ((uint64_t)(io_limit & 0xf0u) << 8u) | (uint64_t)0xfffu;
	bridge->io.size    = range_size(base, limit);
	bridge->io.base    = base;
	bridge->io.present = bridge->io.size != 0u;

	base                   = (uint64_t)(memory_base & 0xfff0u) << 16u;
	limit                  = ((uint64_t)(memory_limit & 0xfff0u) << 16u) | (uint64_t)0xfffffu;
	bridge->memory.size    = range_size(base, limit);
	bridge->memory.base    = base;
	bridge->memory.present = bridge->memory.size != 0u;

	prefetchable_base_upper  = 0u;
	prefetchable_limit_upper = 0u;
	if ((prefetchable_base & PCI_BRIDGE_RANGE_TYPE_MASK) == PCI_BRIDGE_MEMORY_RANGE_64BIT ||
	    (prefetchable_limit & PCI_BRIDGE_RANGE_TYPE_MASK) == PCI_BRIDGE_MEMORY_RANGE_64BIT) {
		if (!pci_config_read32(config, address, PCI_BRIDGE_PREFETCHABLE_BASE_UPPER_OFFSET, &prefetchable_base_upper) ||
		    !pci_config_read32(config, address, PCI_BRIDGE_PREFETCHABLE_LIMIT_UPPER_OFFSET, &prefetchable_limit_upper))
			return false;
	}
	base  = ((uint64_t)prefetchable_base_upper << 32u) | ((uint64_t)(prefetchable_base & 0xfff0u) << 16u);
	limit = ((uint64_t)prefetchable_limit_upper << 32u) | ((uint64_t)(prefetchable_limit & 0xfff0u) << 16u) |
	        (uint64_t)0xfffffu;
	bridge->prefetchable_memory.size         = range_size(base, limit);
	bridge->prefetchable_memory.base         = base;
	bridge->prefetchable_memory.present      = bridge->prefetchable_memory.size != 0u;
	bridge->prefetchable_memory.prefetchable = true;
	return true;
}

bool pci_resources_detect(struct pci_config* config, struct pci_address address, uint8_t header_type,
                          struct pci_resources* out_resources) {
	struct pci_resources resources     = {0};
	uint8_t              header_layout = header_type & PCI_HEADER_LAYOUT_MASK;
	uint16_t             disabled_command;
	bool                 success;

	if (config == NULL || out_resources == NULL) return false;
	resources.requester_id =
		(uint16_t)(((uint16_t)address.bus << 8u) | ((uint16_t)address.device << 3u) | address.function);
	if (!pci_config_read16(config, address, PCI_COMMAND_OFFSET, &resources.command) ||
	    !pci_config_read16(config, address, PCI_STATUS_OFFSET, &resources.status) ||
	    !pci_config_read8(config, address, PCI_INTERRUPT_LINE_OFFSET, &resources.intx.line) ||
	    !pci_config_read8(config, address, PCI_INTERRUPT_PIN_OFFSET, &resources.intx.pin))
		return false;
	disabled_command = resources.command & (uint16_t)~(PCI_COMMAND_IO_ENABLE | PCI_COMMAND_MEMORY_ENABLE);
	if (disabled_command != resources.command &&
	    !pci_config_write16(config, address, PCI_COMMAND_OFFSET, disabled_command))
		return false;
	success = detect_bars(config, address, header_layout, &resources) &&
	          detect_rom(config, address, header_layout, &resources.rom);
	if (disabled_command != resources.command &&
	    !pci_config_write16(config, address, PCI_COMMAND_OFFSET, resources.command))
		success = false;
	if (!success || !detect_capabilities(config, address, header_layout, &resources) ||
	    (header_layout == PCI_HEADER_BRIDGE && !detect_bridge(config, address, &resources.bridge)))
		return false;
	*out_resources = resources;
	return true;
}
