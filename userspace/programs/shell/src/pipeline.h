#pragma once

#include <stdbool.h>

struct shell;
struct shell_pipeline;

bool shell_execute_pipeline(struct shell* shell, const struct shell_pipeline* pipeline);
