#include "pci.h"

#include <base/cap.h>
#include <base/hardware/pci.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <kernel/hardware/pci.h>
#include <stddef.h>

#include "../record.h"

#define PCI_RECORD_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_DELEGATE))

static cap_object_id_t pci_object_id = CAP_OBJECT_ID_INVALID;

static syscall_result_t pci_handler(const struct cap_request* request) {
	struct kernel_record_request record_request;
	syscall_result_t             result;

	result = kernel_record_request_decode(request, sizeof(struct pci_controller), &record_request);
	if (result.status != SYSCALL_STATUS_OK) return result;
	switch (record_request.op) {
	case RECORD_OP_COUNT:
		return kernel_record_count_respond(request, kernel_hardware_pci_count());
	case RECORD_OP_GET:
		if (!kernel_hardware_pci_get(record_request.index, request->response)) {
			return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
		}
		return syscall_result_ok(sizeof(struct pci_controller));
	default:
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
}

bool kernel_capability_pci_init(void) {
	if (kernel_hardware_pci_count() == 0u || pci_object_id != CAP_OBJECT_ID_INVALID) return true;
	pci_object_id = cap_object_create_kernel(0u, pci_handler, NULL);
	return pci_object_id != CAP_OBJECT_ID_INVALID;
}

bool kernel_capability_pci_available(void) {
	return pci_object_id != CAP_OBJECT_ID_INVALID && kernel_hardware_pci_count() != 0u;
}

cap_id_t kernel_capability_pci_grant(process_id_t recipient) {
	if (!kernel_capability_pci_available() || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(pci_object_id, recipient, PCI_RECORD_RIGHTS, NULL);
}
