#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Static storage limits for firmware-described controllers. */
#define LOONGARCH64_MAX_FIXED_CONTROLLERS 6u
#define LOONGARCH64_MAX_PCH_PICS 4u

/* Stable HAL domain identifiers for LoongArch interrupt controllers. */
#define LOONGARCH64_DOMAIN_LIOINTC 1u
#define LOONGARCH64_DOMAIN_PCH_PIC_BASE 16u
#define LOONGARCH64_DOMAIN_PCH_LPC 32u
#define LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI 0u
#define LOONGARCH64_MESSAGE_DOMAIN_AVEC 1u
#define LOONGARCH64_MESSAGE_DOMAIN_REDIRECT 2u

/* Delivery namespaces exposed by LoongArch interrupt controllers. */
#define LOONGARCH64_DELIVERY_DOMAIN_VECTOR 1u
#define LOONGARCH64_DELIVERY_DOMAIN_LIOINTC 2u
#define LOONGARCH64_DELIVERY_DOMAIN_PCH_LPC 3u
#define LOONGARCH64_DELIVERY_DOMAIN_AVEC 4u

/* Architectural input, vector, source, and group limits. */
#define LOONGARCH64_CPU_HWI_BASE 2u
#define LOONGARCH64_CPU_HWI_LIMIT 10u
#define EIOINTC_VECTOR_COUNT 256u
#define PCH_PIC_SOURCE_COUNT 64u
#define LIOINTC_SOURCE_COUNT 32u
#define HTVEC_GROUP_COUNT 8u
#define LPC_SOURCE_COUNT 16u
#define AVEC_VECTOR_BASE 16u
#define AVEC_VECTOR_COUNT 256u
#define LOONGARCH64_AVEC_MESSAGE_OFFSET 0x100000u

/* Hardware classes represented by a fixed interrupt-source domain. */
enum fixed_kind {
	FIXED_LIOINTC,
	FIXED_PCH_PIC,
	FIXED_PCH_LPC,
};

/* Firmware description and mapped state for one fixed-source controller. */
struct fixed_controller {
	enum fixed_kind   kind;
	uint32_t          domain;
	uint32_t          source_count;
	uint32_t          vector_base;
	uint32_t          cascade;
	uintptr_t         physical_base;
	uintptr_t         size;
	volatile uint8_t* virtual_base;
	bool              ready;
};

/* Firmware description of the extended I/O interrupt controller. */
struct eiointc_state {
	uint32_t cascade;
	uint32_t vector_count;
	uint8_t  node;
	uint64_t node_map;
	bool     described;
};

/* Firmware description and mapped state for the legacy HT vector controller. */
struct htvec_state {
	uintptr_t         physical_base;
	uintptr_t         size;
	uint8_t           cascades[HTVEC_GROUP_COUNT];
	volatile uint8_t* virtual_base;
	bool              described;
	bool              ready;
};

/* Firmware description of the PCH MSI controller. */
struct pch_msi_state {
	uint64_t address;
	uint32_t first;
	uint32_t count;
	bool     described;
};

/* Discovered fixed-source controllers. */
extern struct fixed_controller fixed_controllers[LOONGARCH64_MAX_FIXED_CONTROLLERS];
/* Number of populated entries in fixed_controllers. */
extern size_t fixed_controller_count;
/* Discovered EIOINTC state. */
extern struct eiointc_state eiointc;
/* Discovered HTVEC state. */
extern struct htvec_state htvec;
/* Discovered PCH-MSI state. */
extern struct pch_msi_state pch_msi;
/* CPU interrupt inputs used by the two LIOINTC parent outputs. */
extern uint8_t lio_cascades[2];
/* LIOINTC sources routed through each parent output. */
extern uint32_t lio_cascade_maps[2];
/* Whether firmware controller discovery has completed. */
extern bool controllers_discovered;
/* BSP target used by controllers without programmable destination routing. */
extern const struct cpu* loongarch64_fixed_target;

/* Disable local interrupts and acquire the shared controller lock. */
struct irq_state loongarch64_controllers_lock(void);

/* Release the shared controller lock and restore local interrupt state. */
void loongarch64_controllers_unlock(struct irq_state state);

/* Map a controller's physical MMIO range into the kernel address space. */
bool loongarch64_map_mmio(uintptr_t physical, uintptr_t size, volatile uint8_t** out);

/* Find a fixed-source controller by HAL domain identifier. */
struct fixed_controller* loongarch64_fixed_by_domain(uint32_t domain);

/* Read a 32-bit little-endian controller register. */
uint32_t loongarch64_mmio_read32(volatile uint8_t* base, uint32_t offset);

/* Read an 8-bit controller register. */
uint8_t loongarch64_mmio_read8(volatile uint8_t* base, uint32_t offset);

/* Write a 32-bit little-endian controller register. */
void loongarch64_mmio_write32(volatile uint8_t* base, uint32_t offset, uint32_t value);

/* Write an 8-bit controller register. */
void loongarch64_mmio_write8(volatile uint8_t* base, uint32_t offset, uint8_t value);
