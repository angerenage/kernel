#include "xhci.h"

#include <base/address_space.h>
#include <base/memory.h>
#include <base/process.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>
#include <system/time.h>

#include "controller.h"
#include "device.h"
#include "transport.h"

/* xHCI specification, capability / operational register layout. */
#define XHCI_CLASS 0x0cu
#define XHCI_SUBCLASS 0x03u
#define XHCI_PROGIF 0x30u
#define XHCI_BAR_MEMORY32 2u
#define XHCI_BAR_MEMORY64 3u
#define XHCI_BAR_RESOURCE "bar_0"
#define XHCI_BAR_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP))
#define XHCI_CAPLENGTH 0x00u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCSPARAMS2 0x08u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u
#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_PORTREGS 0x400u
#define XHCI_PORTSTRIDE 0x10u
#define XHCI_PORTSC 0x00u
#define XHCI_PORTSC_CCS (1u << 0u)
#define XHCI_PORTSC_PED (1u << 1u)
#define XHCI_PORTSC_PP (1u << 9u)
#define XHCI_PORTSC_SPEED_SHIFT 10u
#define XHCI_PORTSC_SPEED_MASK 0x0fu
#define XHCI_MAX_PORTS 255u
#define XHCI_LOG_PORT_LIMIT 8u

struct xhci_mapping {
	cap_id_t          memory_cap;
	cap_id_t          address_space_cap;
	cap_id_t          mapping_cap;
	volatile uint8_t* registers;
	size_t            bar_size;
};

static bool contains(size_t size, size_t offset, size_t width) {
	return offset <= size && width <= size - offset;
}

static uint32_t read32(const struct xhci_mapping* m, size_t offset) {
	return *(volatile uint32_t*)(m->registers + offset);
}

static void release_mapping(struct xhci_mapping* m) {
	if (m->mapping_cap != CAP_ID_INVALID) (void)mapping_unmap(m->mapping_cap);
	if (m->address_space_cap != CAP_ID_INVALID) (void)cap_drop(m->address_space_cap);
	if (m->memory_cap != CAP_ID_INVALID) (void)cap_drop(m->memory_cap);
	*m = (struct xhci_mapping){
		.memory_cap        = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.mapping_cap       = CAP_ID_INVALID,
	};
}

static bool map_bar(cap_id_t device_cap, uint64_t bar_size, uint64_t mapping_offset, struct xhci_mapping* m) {
	struct memory_info                memory;
	struct self_info                  self;
	struct address_space_map_response mapped = {.mapping_cap = CAP_ID_INVALID};
	*m                                       = (struct xhci_mapping){
		.memory_cap        = CAP_ID_INVALID,
		.address_space_cap = CAP_ID_INVALID,
		.mapping_cap       = CAP_ID_INVALID,
	};
	if (bar_size > SIZE_MAX || mapping_offset > SIZE_MAX || mapping_offset > UINT64_MAX - bar_size) return false;
	if (device_resource_acquire(
			device_cap, XHCI_BAR_RESOURCE, sizeof(XHCI_BAR_RESOURCE) - 1u, XHCI_BAR_RIGHTS, &m->memory_cap) !=
	        SYSCALL_STATUS_OK ||
	    memory_info(m->memory_cap, &memory) != SYSCALL_STATUS_OK || memory.memory_type != MEMORY_TYPE_DEVICE ||
	    mapping_offset + bar_size > memory.size || process_self_info(&self) != SYSCALL_STATUS_OK)
		goto fail;
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (self.address_space_cap == CAP_ID_INVALID) goto fail;
	m->address_space_cap = self.address_space_cap;
	if (address_space_map(
			m->address_space_cap, m->memory_cap, MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE, 0u, 0u, 0u, 0u, &mapped) !=
	        SYSCALL_STATUS_OK ||
	    mapped.mapping_cap == CAP_ID_INVALID)
		goto fail;
	m->mapping_cap = mapped.mapping_cap;
	if (mapped.address == 0u) goto fail;
	m->registers = (volatile uint8_t*)(mapped.address + (uintptr_t)mapping_offset);
	m->bar_size  = (size_t)bar_size;
	return true;
fail:
	release_mapping(m);
	return false;
}

static bool probe_controller(const struct xhci_mapping* m, struct xhci_controller* controller) {
	if (!contains(m->bar_size, XHCI_RTSOFF, sizeof(uint32_t))) return false;
	uint32_t cap_header = read32(m, XHCI_CAPLENGTH);
	uint8_t  caplength  = (uint8_t)(cap_header & 0xffu);
	if (caplength < 0x20u || !contains(m->bar_size, caplength + XHCI_USBSTS, sizeof(uint32_t))) {
		fprintf(stderr, "xhci: invalid capability length %u\n", (unsigned)caplength);
		return false;
	}
	uint16_t version      = (uint16_t)(cap_header >> 16u);
	uint32_t hcs1         = read32(m, XHCI_HCSPARAMS1);
	uint32_t hcs2         = read32(m, XHCI_HCSPARAMS2);
	uint32_t hcc1         = read32(m, XHCI_HCCPARAMS1);
	uint32_t dboff        = read32(m, XHCI_DBOFF) & ~3u;
	uint32_t rtsoff       = read32(m, XHCI_RTSOFF) & ~0x1fu;
	uint32_t ports        = (hcs1 >> 24u) & 0xffu;
	uint32_t slots        = hcs1 & 0xffu;
	uint32_t interrupters = (hcs1 >> 8u) & 0x7ffu;
	if (ports == 0u || ports > XHCI_MAX_PORTS || slots == 0u || !contains(m->bar_size, dboff, sizeof(uint32_t)) ||
	    !contains(m->bar_size, rtsoff, 0x24u) ||
	    !contains(m->bar_size,
	              (size_t)caplength + XHCI_PORTREGS + (size_t)(ports - 1u) * XHCI_PORTSTRIDE + XHCI_PORTSC,
	              sizeof(uint32_t))) {
		fprintf(stderr, "xhci: inconsistent register layout (ports=%u slots=%u)\n", ports, slots);
		return false;
	}
	uint32_t command = read32(m, (size_t)caplength + XHCI_USBCMD);
	uint32_t status  = read32(m, (size_t)caplength + XHCI_USBSTS);
	printf("xhci: caphdr=%08x v%x.%02x ports=%u slots=%u intr=%u 64bit=%u\n",
	       cap_header,
	       (unsigned)(version >> 8u),
	       (unsigned)(version & 0xffu),
	       ports,
	       slots,
	       interrupters,
	       (unsigned)(hcc1 & 1u));
	printf("xhci: caplen=%x dboff=%x rtsoff=%x cmd=%08x sts=%08x hcs2=%08x\n",
	       (unsigned)caplength,
	       dboff,
	       rtsoff,
	       command,
	       status,
	       hcs2);
	unsigned connected = 0u;
	unsigned shown     = 0u;
	for (uint32_t index = 0u; index < ports; index++) {
		size_t   offset = (size_t)caplength + XHCI_PORTREGS + (size_t)index * XHCI_PORTSTRIDE;
		uint32_t portsc = read32(m, offset);
		if ((portsc & XHCI_PORTSC_CCS) == 0u) continue;
		connected++;
		if (shown++ < XHCI_LOG_PORT_LIMIT)
			printf("xhci: port %u connected enabled=%u powered=%u speed-id=%u\n",
			       (unsigned)(index + 1u),
			       (unsigned)((portsc & XHCI_PORTSC_PED) != 0u),
			       (unsigned)((portsc & XHCI_PORTSC_PP) != 0u),
			       (unsigned)((portsc >> XHCI_PORTSC_SPEED_SHIFT) & XHCI_PORTSC_SPEED_MASK));
	}
	printf("xhci: connected ports=%u/%u (initial probe)\n", connected, ports);
	if (!contains(m->bar_size, (size_t)rtsoff + 0x20u, 0x20u) || !contains(m->bar_size, dboff, 4u) ||
	    interrupters == 0u)
		return false;
	*controller = (struct xhci_controller){
		.mmio         = m->registers,
		.size         = m->bar_size,
		.op           = caplength,
		.runtime      = rtsoff,
		.doorbell     = dboff,
		.slots        = (uint8_t)slots,
		.ports        = (uint8_t)ports,
		.context_size = (hcc1 & (1u << 2u)) != 0u ? 64u : 32u,
		.addr64       = (hcc1 & 1u) != 0u,
		.dma          = {.source_cap           = CAP_ID_INVALID,
		                 .memory_allocator_cap = CAP_ID_INVALID,
		                 .device_space_cap     = CAP_ID_INVALID,
		                 .binding_cap          = CAP_ID_INVALID,
		                 .process_space_cap    = CAP_ID_INVALID},
		.dcbaa        = {.memory_cap         = CAP_ID_INVALID,
		                 .cpu_mapping_cap    = CAP_ID_INVALID,
		                 .device_mapping_cap = CAP_ID_INVALID},
		.erst = {.memory_cap = CAP_ID_INVALID, .cpu_mapping_cap = CAP_ID_INVALID, .device_mapping_cap = CAP_ID_INVALID},
		.commands = {.dma = {.memory_cap         = CAP_ID_INVALID,
		                     .cpu_mapping_cap    = CAP_ID_INVALID,
		                     .device_mapping_cap = CAP_ID_INVALID}},
		.events   = {.dma = {.memory_cap         = CAP_ID_INVALID,
		                     .cpu_mapping_cap    = CAP_ID_INVALID,
		                     .device_mapping_cap = CAP_ID_INVALID}},
	};
	return true;
}

enum device_driver_result xhci_driver_run(cap_id_t device_cap) {
	uint64_t cls, subclass, interface, kind, size, offset;
	bool     enabled, claimed;
	if (device_property_read_u64(device_cap, "class_code", &cls) != SYSCALL_STATUS_OK ||
	    device_property_read_u64(device_cap, "subclass", &subclass) != SYSCALL_STATUS_OK ||
	    device_property_read_u64(device_cap, "programming_interface", &interface) != SYSCALL_STATUS_OK)
		return DEVICE_DRIVER_ERROR;
	if (cls != XHCI_CLASS || subclass != XHCI_SUBCLASS || interface != XHCI_PROGIF) return DEVICE_DRIVER_UNSUPPORTED;
	syscall_status_t statuses[] = {
		device_property_read_u64(device_cap, "bar_0_kind", &kind),
		device_property_read_u64(device_cap, "bar_0_size", &size),
		device_property_read_u64(device_cap, "bar_0_mapping_offset", &offset),
		device_property_read_bool(device_cap, "bar_0_enabled", &enabled),
		device_property_read_bool(device_cap, "bar_0_claimed", &claimed),
	};
	bool missing = false;
	for (size_t i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		if (statuses[i] == SYSCALL_STATUS_UNAVAILABLE) missing = true;
		else if (statuses[i] != SYSCALL_STATUS_OK) return DEVICE_DRIVER_ERROR;
	}
	if (missing || (kind != XHCI_BAR_MEMORY32 && kind != XHCI_BAR_MEMORY64) || !enabled || !claimed || size < 0x500u) {
		fprintf(stderr, "xhci: BAR0 missing, disabled or unclaimed\n");
		return DEVICE_DRIVER_UNSUPPORTED;
	}
	struct xhci_mapping mapping;
	if (!map_bar(device_cap, size, offset, &mapping)) {
		fprintf(stderr, "xhci: unable to map BAR0\n");
		return DEVICE_DRIVER_ERROR;
	}
	struct xhci_controller controller;
	bool                   ok = probe_controller(&mapping, &controller);
	if (!ok) {
		release_mapping(&mapping);
		return DEVICE_DRIVER_ERROR;
	}

	if (!xhci_controller_start(&controller, device_cap)) {
		xhci_controller_stop(&controller);
		release_mapping(&mapping);
		return DEVICE_DRIVER_ERROR;
	}
	if (!xhci_device_poll(&controller)) {
		xhci_controller_stop(&controller);
		release_mapping(&mapping);
		return DEVICE_DRIVER_ERROR;
	}
	struct xhci_transport transport;
	if (!xhci_transport_init(&transport, &controller, device_cap)) {
		xhci_transport_release(&transport);
		xhci_controller_stop(&controller);
		release_mapping(&mapping);
		return DEVICE_DRIVER_ERROR;
	}
	for (;;) {
		if (!xhci_controller_poll(&controller) || !xhci_device_poll(&controller) || !xhci_transport_poll(&transport) ||
		    sleep_ms(10u) != SYSCALL_STATUS_OK) {
			xhci_transport_release(&transport);
			xhci_controller_stop(&controller);
			release_mapping(&mapping);
			return DEVICE_DRIVER_ERROR;
		}
	}
}
