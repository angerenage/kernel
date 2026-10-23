#include "pci.h"

#include <stdio.h>
#include <system/time.h>

#include "enumerate.h"
#include "host.h"

bool pci_driver_run(cap_id_t device_cap) {
	struct pci_host host;

	if (!pci_host_initialize(device_cap, &host)) {
		static const char message[] = "pci: invalid or missing ECAM host resources\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
		pci_host_deinitialize(&host);
		return false;
	}
	if (!pci_enumerate(&host)) {
		static const char message[] = "pci: configuration-space enumeration failed\n";
		(void)fwrite(message, 1u, sizeof(message) - 1u, stderr);
		pci_host_deinitialize(&host);
		return false;
	}
	for (;;)
		if (sleep_ms(1000u) != SYSCALL_STATUS_OK) {
			pci_host_deinitialize(&host);
			return false;
		}
}
