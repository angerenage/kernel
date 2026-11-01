#pragma once

#include <base/cap.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum foreground pipeline: 16 processes and 15 pipe connections. */
#define SHELL_PIPE_MAX 15u

struct shell_pipe_pair {
	cap_id_t read_cap;
	cap_id_t write_cap;
};

/* Begin a new set of independent bounded FIFO byte streams. */
bool shell_pipe_begin(size_t count, struct shell_pipe_pair pairs[SHELL_PIPE_MAX]);

/* Close the shell's grant after handing an endpoint to its stage. */
void shell_pipe_drop(cap_id_t* cap);

/* Explicitly close a pipe reader with an EOF signal. */
bool shell_pipe_close_reader(size_t index);

/* Explicitly close a pipe writer with an EOF signal. */
bool shell_pipe_close_writer(size_t index);

/* Only call after every child using a pipe has exited. */
bool shell_pipe_finish(void);
