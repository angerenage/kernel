#pragma once

#include <base/channel.h>
#include <base/syscall.h>

#include "load.h"

/* Unpublish a terminal load regardless of its current grants or calls. */
syscall_status_t loader_unpublish_terminal(channel_id_t endpoint, struct loader_loaded_program* program);

/* Unpublish an abandoned load only while it remains unused. */
bool loader_unpublish_abandoned(channel_id_t endpoint, struct loader_loaded_program* program);
