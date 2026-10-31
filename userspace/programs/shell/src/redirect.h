#pragma once

#include <base/cap.h>
#include <stdbool.h>

struct shell;
struct shell_command;

/* The stream caps remain owned by the shell and are borrowed until finish(). */
struct shell_redirect_streams {
	cap_id_t input;
	cap_id_t output;
	bool     input_changed;
	bool     output_changed;
};

/* Prepare for redirection of the given command's input and output streams. */
bool shell_redirect_prepare(const struct shell* shell, const struct shell_command* command,
                            struct shell_redirect_streams* out_streams, const char** error_path,
                            const char** error_message);

/* Finish the redirection and clean up resources. */
bool shell_redirect_finish(void);
