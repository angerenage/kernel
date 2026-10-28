#include "pci.h"

#include <stdio.h>
#include <system/time.h>
#include <system/trace.h>

#include "device.h"
#include "enumerate.h"
#include "host.h"

TRACE_LABEL(trace_host_initialize, "pci/host_initialize");
TRACE_LABEL(trace_enumerate, "pci/enumerate");
TRACE_LABEL(trace_publish, "pci/publish");

enum device_driver_result pci_driver_run(cap_id_t device_cap, cap_id_t memory_allocator_cap, cap_id_t dma_cap,
                                         cap_id_t interrupts_cap, cap_id_t io_ports_cap) {
	struct pci_host      host;
	struct pci_inventory inventory = {0};
	bool                 success;

	{
		TRACE_SCOPE(trace, &trace_host_initialize);
		success = pci_host_initialize(device_cap, &host);
	}
	if (!success) {
		static const char message[] = "pci: invalid or missing ECAM host resources\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
		pci_host_deinitialize(&host);
		return DEVICE_DRIVER_ERROR;
	}
	{
		TRACE_SCOPE(trace, &trace_enumerate);
		success = pci_enumerate(&host, memory_allocator_cap, interrupts_cap, io_ports_cap, &inventory);
	}
	if (!success) {
		static const char message[] = "pci: configuration-space enumeration failed\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
		pci_host_deinitialize(&host);
		return DEVICE_DRIVER_ERROR;
	}
	syscall_status_t publish_status;
	{
		TRACE_SCOPE(trace, &trace_publish);
		publish_status = pci_devices_publish(device_cap, memory_allocator_cap, dma_cap, &inventory);
	}
	if (publish_status != SYSCALL_STATUS_OK) {
		fprintf(stderr, "pci: could not publish the discovered functions: %u\n", (unsigned)publish_status);
		pci_inventory_deinitialize(&inventory);
		pci_host_deinitialize(&host);
		return DEVICE_DRIVER_ERROR;
	}
	printf("pci: published %zu function(s) with %zu claimed resource(s)\n",
	       inventory.function_count,
	       inventory.resource_count);
	for (;;)
		if (sleep_ms(1000u) != SYSCALL_STATUS_OK) {
			pci_inventory_deinitialize(&inventory);
			pci_host_deinitialize(&host);
			return DEVICE_DRIVER_ERROR;
		}
}
