#pragma once

#include <base/cap.h>
#include <stdint.h>

/*
 * Optional kernel command-line override forwarded verbatim by init.
 * The default and "auto" prefer a valid Device Tree and otherwise use ACPI.
 */
#define DEVICE_MANAGER_SOURCE_OPTION "device_manager.source"
#define DEVICE_MANAGER_SOURCE_AUTO "auto"
#define DEVICE_MANAGER_SOURCE_DEVICE_TREE "dt"
#define DEVICE_MANAGER_SOURCE_ACPI "acpi"

/* Positional capabilities accepted by the device manager at startup. */
enum device_manager_capability_argument {
	DEVICE_MANAGER_CAPABILITY_ACPI = 0u,
	DEVICE_MANAGER_CAPABILITY_DEVICE_TREE,
	DEVICE_MANAGER_CAPABILITY_MEMORY_ALLOCATOR,
	DEVICE_MANAGER_CAPABILITY_DMA,
	DEVICE_MANAGER_CAPABILITY_INTERRUPTS,
	DEVICE_MANAGER_CAPABILITY_IO_PORTS,
	DEVICE_MANAGER_CAPABILITY_COUNT,
};

/* Positional capabilities passed by the device manager to its selected parser. */
enum device_parser_capability_argument {
	DEVICE_PARSER_CAPABILITY_FIRMWARE = 0u,
	DEVICE_PARSER_CAPABILITY_DEVICE_ROOT,
	DEVICE_PARSER_CAPABILITY_MEMORY_ALLOCATOR,
	DEVICE_PARSER_CAPABILITY_DMA,
	DEVICE_PARSER_CAPABILITY_INTERRUPTS,
	DEVICE_PARSER_CAPABILITY_IO_PORTS,
	DEVICE_PARSER_CAPABILITY_COUNT,
};

/* Exact rights delegated to a parser for constructing root devices. */
#define DEVICE_PARSER_ROOT_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_MANAGE))

/* Rights retained by the manager while forwarding physical-memory claim authority. */
#define DEVICE_MANAGER_MEMORY_ALLOCATOR_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Rights required by a parser to inspect the allocator and claim discovered MMIO ranges. */
#define DEVICE_PARSER_MEMORY_ALLOCATOR_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE))

/* Rights retained by the manager while forwarding DMA-source claim authority. */
#define DEVICE_MANAGER_DMA_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Rights required by a parser to resolve and claim firmware-described DMA sources. */
#define DEVICE_PARSER_DMA_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE))

/* Rights retained by the manager while forwarding fixed-interrupt claim authority. */
#define DEVICE_MANAGER_INTERRUPTS_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Rights required by a parser to resolve and claim firmware-described fixed interrupts. */
#define DEVICE_PARSER_INTERRUPTS_CAP_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE))

/* Rights required to derive driver-facing I/O-port ranges from the optional x86 provider. */
#define DEVICE_PARSER_IO_PORTS_CAP_RIGHTS                                                                              \
	((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP | CAP_DERIVE | CAP_DELEGATE))
