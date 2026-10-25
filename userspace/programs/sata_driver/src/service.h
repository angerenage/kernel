#pragma once

#include <base/cap.h>
#include <stdbool.h>

struct sata_device_info;
struct sata_transport;

/* Publish a block:disk child and serve controller-independent block requests. */
bool sata_block_service_run(cap_id_t device_cap, const struct sata_transport* transport,
                            const struct sata_device_info* info);
