#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "init.h"

/* Validate argc consecutive NUL-terminated strings occupying argv_size bytes. */
bool init_arguments_valid(uint32_t argc, const char* argv_data, size_t argv_size);

/* Build a temporary pointer vector over init's retained argument payload. */
bool init_arguments_vector(const struct init_state* init, const char*** out_argv);
