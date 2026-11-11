#include "pipeline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/process.h>

#include <system/capability.h>
#include <system/display.h>
#include <system/process.h>
#include <system/thread.h>
#include <runtime/tty_session.h>

#include "parse.h"
#include "pipe.h"
#include "redirect.h"
#include "shell.h"

struct stage {
	cap_id_t input;
	cap_id_t output;
	cap_id_t process;
	cap_id_t waiter;
	syscall_status_t wait_status;
	uintptr_t exit_code;
	size_t index;
	size_t count;
	struct stage* group;
	bool builtin;
};

/* Each stage is reaped independently: waiting for the producer first can
 * deadlock when the consumer exits early; waiting for the consumer first can
 * deadlock when it needs an EOF from the producer. */
static void stage_wait_worker(void* argument) {
	struct stage* stage = *(struct stage**)argument;
	uintptr_t code = PROCESS_EXIT_SYSTEM_UNKNOWN;
	stage->wait_status = process_wait(stage->process, &code);
	stage->exit_code = code;
	if (stage->wait_status != SYSCALL_STATUS_OK) (void)cap_drop(stage->process);
	__atomic_store_n(&stage->process, CAP_ID_INVALID, __ATOMIC_RELEASE);
	/* The TTY session can only interrupt one foreground process. Cancel the
	 * other stages if that process was killed by Ctrl+C. */
	if (stage->wait_status == SYSCALL_STATUS_OK && code == PROCESS_EXIT_SYSTEM_INTERRUPTED) {
		for (size_t i = 0u; i < stage->count; i++) {
			if (i == stage->index) continue;
			cap_id_t sibling = __atomic_load_n(&stage->group[i].process, __ATOMIC_ACQUIRE);
			if (sibling != CAP_ID_INVALID) (void)process_kill(sibling, PROCESS_EXIT_SYSTEM_INTERRUPTED);
		}
	}
	/* An explicit close is essential to wake blocked stream readers/writers,
	 * independently of the kernel's zero-grants notification schedule. */
	bool ok = true;
	if (stage->index != 0u && !shell_pipe_close_reader(stage->index - 1u)) ok = false;
	if (stage->index + 1u < stage->count && !shell_pipe_close_writer(stage->index)) ok = false;
	thread_exit(stage->wait_status == SYSCALL_STATUS_OK && ok ? 0u : 1u);
}

static void diagnostic(const char* command, const char* message) {
	if (command == NULL) (void)fprintf(stderr, "\x1b[1;31mshell:\x1b[0m %s\n", message);
	else (void)fprintf(stderr, "\x1b[1;31mshell:\x1b[0m %s: %s\n", command, message);
}

static void drop_stage_ends(struct shell_pipe_pair* pairs, size_t index, size_t count) {
	if (index != 0u) shell_pipe_drop(&pairs[index - 1u].read_cap);
	if (index + 1u != count) shell_pipe_drop(&pairs[index].write_cap);
}

bool shell_execute_pipeline(struct shell* shell, const struct shell_pipeline* pipeline) {
	const size_t count = pipeline->command_count;
	struct shell_pipe_pair pairs[SHELL_PIPE_MAX] = {0};
	struct stage* stages = NULL;
	cap_id_t terminal_input, terminal_output, terminal_error;
	size_t foreground_index = count;
	bool foreground = false, started_pipes = false, ok = true, aborted = false, interrupted = false;
	struct self_info self = {
		.self_cap = CAP_ID_INVALID, .address_space_cap = CAP_ID_INVALID, .main_thread_cap = CAP_ID_INVALID
	};

	if (count < 2u || count > SHELL_PIPE_MAX + 1u) {
		diagnostic(NULL, "too many commands in pipeline");
		return true;
	}
	for (size_t i = 0u; i < count; i++) {
		const struct shell_command* command = &pipeline->commands[i];
		if (command->argc == 0u) {
			diagnostic(NULL, "missing pipeline command");
			return true;
		}
		if (shell_is_builtin(command->argv[0])) {
			if (strcmp(command->argv[0], "cd") == 0 || strcmp(command->argv[0], "exit") == 0) {
				diagnostic(command->argv[0], "not supported inside a pipeline");
				return true;
			}
		}
		else foreground_index = i;
	}
	stages = calloc(count, sizeof(*stages));
	if (stages == NULL) {
		diagnostic(NULL, "out of memory");
		return true;
	}
	for (size_t i = 0u; i < count; i++) {
		stages[i].process = CAP_ID_INVALID;
		stages[i].waiter = CAP_ID_INVALID;
		stages[i].wait_status = SYSCALL_STATUS_FAILED;
		stages[i].exit_code = PROCESS_EXIT_SYSTEM_UNKNOWN;
		stages[i].index = i;
		stages[i].count = count;
		stages[i].group = stages;
	}
	for (size_t i = 0u; i < SHELL_PIPE_MAX; i++)
		pairs[i] = (struct shell_pipe_pair){.read_cap = CAP_ID_INVALID, .write_cap = CAP_ID_INVALID};
	if (!shell_redirect_finish()) {
		diagnostic(NULL, "could not reset redirected streams");
		goto cleanup;
	}
	if (!shell_pipe_begin(count - 1u, pairs)) {
		diagnostic(NULL, "unable to initialize pipe streams");
		goto cleanup;
	}
	started_pipes = true;
	display_get_standard_streams(&terminal_input, &terminal_output, &terminal_error);
	for (size_t i = 0u; i < count; i++) {
		const struct shell_command* command = &pipeline->commands[i];
		struct shell_redirect_streams redirects = {0};
		const char* error_path;
		const char* error_message;
		stages[i].builtin = shell_is_builtin(command->argv[0]);
		stages[i].input = i == 0u ? terminal_input : pairs[i - 1u].read_cap;
		stages[i].output = i + 1u == count ? terminal_output : pairs[i].write_cap;
		if (command->redirection_count == 0u) continue;
		if (!shell_redirect_prepare(shell, command, &redirects, &error_path, &error_message)) {
			diagnostic(error_path, error_message);
			aborted = true;
			goto cleanup;
		}
		if (redirects.input_changed) stages[i].input = redirects.input;
		if (redirects.output_changed) stages[i].output = redirects.output;
	}

	if (process_self_info(&self) != SYSCALL_STATUS_OK || self.self_cap == CAP_ID_INVALID) {
		diagnostic(NULL, "unable to initialize pipeline waiters");
		aborted = true;
		goto cleanup;
	}
	if (self.address_space_cap != CAP_ID_INVALID) {
		(void)cap_drop(self.address_space_cap);
		self.address_space_cap = CAP_ID_INVALID;
	}
	if (self.main_thread_cap != CAP_ID_INVALID) {
		(void)cap_drop(self.main_thread_cap);
		self.main_thread_cap = CAP_ID_INVALID;
	}
	for (size_t i = 0u; i < count; i++) {
		const struct shell_command* command = &pipeline->commands[i];
		if (stages[i].builtin) continue;
		enum shell_program_result result = shell_pipeline_start_program(shell,
				command->argv[0],
				command->argc - 1u,
				(const char* const*)(command->argv + 1u),
				stages[i].input,
				stages[i].output,
				terminal_error,
				i == foreground_index,
				&stages[i].process);
		if (result != SHELL_PROGRAM_OK) {
			diagnostic(command->argv[0],
					result == SHELL_PROGRAM_NOT_FOUND ? "command not found" : "unable to launch program");
			aborted = true;
			goto cleanup;
		}
		if (i == foreground_index) foreground = true;
		if (i != 0u && stages[i].input != pairs[i - 1u].read_cap && !shell_pipe_close_reader(i - 1u)) ok = false;
		if (i + 1u != count && stages[i].output != pairs[i].write_cap && !shell_pipe_close_writer(i)) ok = false;
		drop_stage_ends(pairs, i, count);
		struct stage* stage = &stages[i];
		if (process_spawn_thread(
					self.self_cap, (uintptr_t)stage_wait_worker, &stage, sizeof(stage), NULL, 0u, &stages[i].waiter) !=
				SYSCALL_STATUS_OK) {
			diagnostic(command->argv[0], "unable to start process waiter");
			aborted = true;
			goto cleanup;
		}
	}
	for (size_t i = 0u; i < count; i++) {
		const struct shell_command* command = &pipeline->commands[i];
		if (!stages[i].builtin) continue;
		cap_id_t input, output, error;
		display_get_standard_streams(&input, &output, &error);
		display_set_standard_streams(stages[i].input, stages[i].output, error);
		clearerr(stdin);
		clearerr(stdout);
		bool handled = false;
		bool result = shell_execute_builtin(shell, command, &handled);
		display_set_standard_streams(input, output, error);
		clearerr(stdin);
		clearerr(stdout);
		drop_stage_ends(pairs, i, count);
		if (i != 0u && !shell_pipe_close_reader(i - 1u)) ok = false;
		if (i + 1u != count && !shell_pipe_close_writer(i)) ok = false;
		if (!result || !handled) {
			diagnostic(command->argv[0], "builtin failed");
			ok = false;
		}
	}

cleanup:
	if (self.self_cap != CAP_ID_INVALID) (void)cap_drop(self.self_cap);
	if (self.address_space_cap != CAP_ID_INVALID) (void)cap_drop(self.address_space_cap);
	if (self.main_thread_cap != CAP_ID_INVALID) (void)cap_drop(self.main_thread_cap);
	if (aborted) {
		for (size_t i = 0u; i < count; i++) {
			cap_id_t process = __atomic_load_n(&stages[i].process, __ATOMIC_ACQUIRE);
			if (process != CAP_ID_INVALID) (void)process_kill(process, PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED);
		}
	}
	for (size_t i = 0u; i < count; i++) {
		if (stages[i].waiter != CAP_ID_INVALID) {
			uintptr_t waiter_exit = 1u;
			if (thread_join(stages[i].waiter, &waiter_exit) != SYSCALL_STATUS_OK || waiter_exit != 0u) {
				if (!aborted) diagnostic(pipeline->commands[i].argv[0], "unable to reap pipeline stage");
				ok = false;
			}
			stages[i].waiter = CAP_ID_INVALID; /* Joining consumes the thread capability. */
		}
		else if (stages[i].process != CAP_ID_INVALID) {
			uintptr_t exit_code;
			stages[i].wait_status = process_wait(stages[i].process, &exit_code);
			stages[i].exit_code = exit_code;
			if (stages[i].wait_status != SYSCALL_STATUS_OK) (void)cap_drop(stages[i].process);
			stages[i].process = CAP_ID_INVALID;
			if (i != 0u && !shell_pipe_close_reader(i - 1u)) ok = false;
			if (i + 1u < count && !shell_pipe_close_writer(i)) ok = false;
		}
		if (stages[i].builtin) continue;
		if (stages[i].wait_status != SYSCALL_STATUS_OK) {
			if (!aborted) diagnostic(pipeline->commands[i].argv[0], "unable to wait for process");
			ok = false;
			continue;
		}
		if (!aborted && stages[i].exit_code != PROCESS_EXIT_SUCCESS) {
			if (stages[i].exit_code == PROCESS_EXIT_SYSTEM_INTERRUPTED) interrupted = true;
			if (interrupted) continue;
			char message[72];
			(void)snprintf(message,
					sizeof(message),
					"exited with code %llu (0x%llx)",
					(unsigned long long)stages[i].exit_code,
					(unsigned long long)stages[i].exit_code);
			diagnostic(pipeline->commands[i].argv[0], message);
			ok = false;
		}
	}
	if (foreground && tty_session_restore_foreground(shell->tty_cap) != SYSCALL_STATUS_OK) {
		diagnostic(NULL, "unable to restore foreground terminal");
		ok = false;
	}
	for (size_t i = 0u; i < count - 1u; i++) {
		shell_pipe_drop(&pairs[i].read_cap);
		shell_pipe_drop(&pairs[i].write_cap);
	}
	if (started_pipes && !shell_pipe_finish()) {
		diagnostic(NULL, "unable to release pipe streams");
		ok = false;
	}
	if (!shell_redirect_finish()) {
		diagnostic(NULL, "redirected stream I/O or cleanup failed");
		ok = false;
	}
	free(stages);
	(void)ok;
	return true;
}
