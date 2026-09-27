#include <kernel/hardware/pci.h>
#include <stddef.h>
#include <string.h>

#define HARDWARE_MOCK_MAX_PCI_CONTROLLERS 8u

static struct pci_controller mock_pci_controllers[HARDWARE_MOCK_MAX_PCI_CONTROLLERS];
static size_t                mock_pci_controller_count;

void hardware_mock_set_pci_controllers(const struct pci_controller* controllers, size_t count) {
	if (controllers == NULL || count > HARDWARE_MOCK_MAX_PCI_CONTROLLERS) {
		mock_pci_controller_count = 0u;
		return;
	}
	memcpy(mock_pci_controllers, controllers, count * sizeof(*controllers));
	mock_pci_controller_count = count;
}

void hardware_mock_reset(void) {
	memset(mock_pci_controllers, 0, sizeof(mock_pci_controllers));
	mock_pci_controller_count = 0u;
}

size_t kernel_hardware_pci_count(void) {
	return mock_pci_controller_count;
}

bool kernel_hardware_pci_get(size_t index, struct pci_controller* out_controller) {
	if (index >= mock_pci_controller_count || out_controller == NULL) return false;
	*out_controller = mock_pci_controllers[index];
	return true;
}
