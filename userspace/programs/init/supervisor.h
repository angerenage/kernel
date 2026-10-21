#pragma once

#include <base/process.h>
#include <runtime/registry.h>
#include <stdbool.h>

#include "init.h"

/* Start and supervise the foundational services and managers. */
int supervisor_run(const struct init_state* init);
