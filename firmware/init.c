#include <firmware/acpi.h>
#include <firmware/dt.h>
#include <firmware/init.h>

bool firmware_init(const struct boot_info* info) {
	if (info == NULL) return false;
	if (info->rsdp_address != 0u && !acpi_init(info)) return false;
	if (info->dtb_address != 0u && !dt_init(info)) return false;
	return true;
}
