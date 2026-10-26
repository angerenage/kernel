#pragma once

#include <runtime/program.h>
#include <stdbool.h>
#include <stddef.h>

#include "init.h"

/* Start the registry and receive its delegable service capability. */
bool bootstrap_launch_registry(struct init_state* init);

/* Load and start one supervised userspace boot module with arguments and application capabilities. */
bool bootstrap_launch(const struct init_state* init, const char* module_name, const char* description, size_t argc,
                      const char* const argv[], size_t capability_count,
                      const struct program_capability_argument capabilities[], cap_id_t* out_process_cap);

/* Launch the TTY session with the system terminal connected to standard input. */
bool bootstrap_launch_terminal_session(const struct init_state* init, const char* module_name, const char* description,
                                       size_t argc, const char* const argv[], size_t capability_count,
                                       const struct program_capability_argument capabilities[],
                                       cap_id_t*                                out_process_cap);

/* Launch a program with the TTY session as delegable standard input and output. */
bool bootstrap_launch_interactive(const struct init_state* init, const char* module_name, const char* description,
                                  size_t argc, const char* const argv[], size_t capability_count,
                                  const struct program_capability_argument capabilities[], cap_id_t* out_process_cap);

/* Load and start one installed program from /bin through VFS and the userspace loader. */
bool installed_launch(const struct init_state* init, const char* file_name, const char* description, size_t argc,
                      const char* const argv[], size_t capability_count,
                      const struct program_capability_argument capabilities[], cap_id_t* out_process_cap);

/* Launch an installed TTY session with the terminal connected to standard input. */
bool installed_launch_terminal_session(const struct init_state* init, const char* file_name, const char* description,
                                       size_t argc, const char* const argv[], size_t capability_count,
                                       const struct program_capability_argument capabilities[],
                                       cap_id_t*                                out_process_cap);

/* Launch an installed interactive program under the active TTY session. */
bool installed_launch_interactive(const struct init_state* init, const char* file_name, const char* description,
                                  size_t argc, const char* const argv[], size_t capability_count,
                                  const struct program_capability_argument capabilities[], cap_id_t* out_process_cap);
