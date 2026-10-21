#pragma once

#include <base/cap.h>
#include <base/process.h>

/* Publish the registry to init, report its capability, and serve requests. */
int registry_server_run(process_id_t init_pid, cap_id_t ready_cap);
