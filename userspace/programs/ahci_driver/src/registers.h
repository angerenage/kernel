#pragma once

#include <stdint.h>

#define AHCI_GENERIC_REGISTERS_SIZE 0x2cu
#define AHCI_PORT_REGISTERS_OFFSET 0x100u
#define AHCI_PORT_REGISTERS_SIZE 0x80u
#define AHCI_PORT_COUNT 32u

#define AHCI_CAP_NP_MASK 0x1fu
#define AHCI_CAP_NCS_MASK (0x1fu << 8u)
#define AHCI_CAP_NCS_SHIFT 8u
#define AHCI_CAP_ISS_MASK (0x0fu << 20u)
#define AHCI_CAP_ISS_SHIFT 20u
#define AHCI_CAP_SNCQ (1u << 30u)
#define AHCI_CAP_S64A (1u << 31u)

#define AHCI_GHC_AE (1u << 31u)

#define AHCI_PORT_SSTS_DET_MASK 0x0fu
#define AHCI_PORT_SSTS_IPM_MASK (0x0fu << 8u)
#define AHCI_PORT_SSTS_IPM_SHIFT 8u

#define AHCI_PORT_SIGNATURE_ATA 0x00000101u
#define AHCI_PORT_SIGNATURE_ATAPI 0xeb140101u
#define AHCI_PORT_SIGNATURE_SEMB 0xc33c0101u
#define AHCI_PORT_SIGNATURE_MULTIPLIER 0x96690101u

/* AHCI generic host-control register block. */
struct ahci_host_registers {
	uint32_t capabilities;
	uint32_t global_host_control;
	uint32_t interrupt_status;
	uint32_t ports_implemented;
	uint32_t version;
	uint32_t command_completion_control;
	uint32_t command_completion_ports;
	uint32_t enclosure_management_location;
	uint32_t enclosure_management_control;
	uint32_t capabilities_extended;
	uint32_t bios_handoff_control;
};

/* Registers used while identifying one implemented AHCI port. */
struct ahci_port_registers {
	uint32_t command_list_base_low;
	uint32_t command_list_base_high;
	uint32_t fis_base_low;
	uint32_t fis_base_high;
	uint32_t interrupt_status;
	uint32_t interrupt_enable;
	uint32_t command;
	uint32_t reserved_0;
	uint32_t task_file_data;
	uint32_t signature;
	uint32_t sata_status;
	uint32_t sata_control;
	uint32_t sata_error;
	uint32_t sata_active;
	uint32_t command_issue;
	uint32_t sata_notification;
	uint32_t fis_based_switching;
	uint32_t device_sleep;
	uint8_t  reserved_1[0x70u - 0x48u];
	uint8_t  vendor[0x80u - 0x70u];
};

_Static_assert(sizeof(struct ahci_host_registers) == AHCI_GENERIC_REGISTERS_SIZE,
               "AHCI host register layout must match the specification");
_Static_assert(sizeof(struct ahci_port_registers) == AHCI_PORT_REGISTERS_SIZE,
               "AHCI port register layout must match the specification");
