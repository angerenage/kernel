#include "sata.h"

#include <system/time.h>

#include "identify.h"
#include "transport.h"

bool sata_driver_run(cap_id_t device_cap) {
	struct sata_transport transport = {.root_cap = CAP_ID_INVALID, .session_cap = CAP_ID_INVALID};
	bool                  success   = sata_transport_initialize(device_cap, &transport);

	if (success) success = sata_identify_device(&transport);
	while (success) success = sleep_ms(1000u) == SYSCALL_STATUS_OK;
	sata_transport_deinitialize(&transport);
	return success;
}
