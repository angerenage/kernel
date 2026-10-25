#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Resolve a PCI requester through the ACPI IORT mapping graph. */
bool aarch64_iort_pci_route(uint16_t segment, uint16_t requester_id, uint64_t* out_controller_address,
                            uint32_t* out_source_id);
