#pragma once

#include <base/cap.h>
#include <stdbool.h>

#include "server.h"
#include "source.h"

/* Launch the selected parser and consume the manager's firmware and root-construction grants on success. */
bool device_manager_parser_launch(struct device_server* server, enum device_manager_firmware_source source,
                                  cap_id_t* firmware_cap, cap_id_t memory_allocator_cap, cap_id_t io_ports_cap);
