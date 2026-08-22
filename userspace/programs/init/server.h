#pragma once

#include <base/process.h>
#include <base/startup.h>
#include <runtime/init.h>
#include <stdbool.h>

/* Create init's service channel. */
bool server_init(void);

/* Destroy init's service channel if it was created. */
void server_deinit(void);

/* Grant a process permission to call and delegate the init service capability. */
syscall_status_t init_server_grant(process_id_t target, cap_id_t* out_cap);

/* Start the loader once the registry is ready, then serve registry requests. */
int server_run(struct init_startup_info* startup);
