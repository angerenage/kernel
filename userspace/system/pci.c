#include <protocol/pci.h>
#include <system/capability.h>
#include <system/pci.h>

syscall_status_t pci_control_set_dma_enabled(cap_id_t control_cap, bool enabled, uint16_t* out_command) {
	const struct pci_control_set_dma_enabled_request request = {
		.header  = {.operation = PCI_CONTROL_OP_SET_DMA_ENABLED},
		.enabled = enabled ? 1u : 0u,
	};
	struct pci_control_set_dma_enabled_response response;
	size_t                                      response_size;
	syscall_status_t                            status =
		cap_call(control_cap, &request, sizeof(request), &response, sizeof(response), &response_size);
	if (status != SYSCALL_STATUS_OK) return status;
	if (response_size != sizeof(response)) return SYSCALL_STATUS_FAILED;
	if (out_command != NULL) *out_command = response.command;
	return SYSCALL_STATUS_OK;
}
