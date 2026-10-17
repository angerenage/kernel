#pragma once

#include <base/cap.h>
#include <stdbool.h>

struct shell;
struct shell_foreground_input;

/* Create the shell-owned Stream capability used as one foreground program's standard input. */
struct shell_foreground_input* shell_foreground_input_create(struct shell* shell);

/* Return the delegable source capability for the foreground program's standard input. */
cap_id_t shell_foreground_input_capability(const struct shell_foreground_input* input);

/* Start terminal reading and Stream request handling for the foreground process. */
bool shell_foreground_input_start(struct shell_foreground_input* input, cap_id_t process_cap);

/* Stop both foreground-input workers and release the proxy channel. */
bool shell_foreground_input_destroy(struct shell_foreground_input* input);
