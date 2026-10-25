#pragma once

#include <stdbool.h>

struct sata_transport;

/* Issue IDENTIFY DEVICE and report the disk identity and basic capabilities. */
bool sata_identify_device(const struct sata_transport* transport);
