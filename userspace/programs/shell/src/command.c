#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "parse.h"
#include "shell.h"

#define SHELL_ERROR_PREFIX "\x1b[1;31mshell:\x1b[0m "

static bool write_error(const char* command, const char* message) {
	if (message == NULL) return false;
	if (command == NULL) return printf(SHELL_ERROR_PREFIX "%s\n", message) >= 0;
	return printf(SHELL_ERROR_PREFIX "%s: %s\n", command, message) >= 0;
}

static bool write_unknown_command(const char* command) {
	return command != NULL && printf(SHELL_ERROR_PREFIX "command not found: %s\n", command) >= 0;
}

static bool write_program_exit(const char* command, uintptr_t exit_code) {
	return command != NULL && printf(SHELL_ERROR_PREFIX "%s: exited with code %llu (0x%llx)\n",
	                                 command,
	                                 (unsigned long long)exit_code,
	                                 (unsigned long long)exit_code) >= 0;
}

static bool report_change_directory(enum shell_change_directory_result result) {
	switch (result) {
	case SHELL_CHANGE_DIRECTORY_OK:
		return true;
	case SHELL_CHANGE_DIRECTORY_INVALID_PATH:
		return write_error("cd", "invalid path");
	case SHELL_CHANGE_DIRECTORY_NOT_FOUND:
		return write_error("cd", "directory not found");
	case SHELL_CHANGE_DIRECTORY_NOT_DIRECTORY:
		return write_error("cd", "not a directory");
	case SHELL_CHANGE_DIRECTORY_DENIED:
		return write_error("cd", "permission denied");
	case SHELL_CHANGE_DIRECTORY_FAILED:
	default:
		return write_error("cd", "filesystem operation failed");
	}
}

static bool execute_change_directory(struct shell* shell, size_t argc, char* const argv[]) {
	if (argc == 0u) return write_error("cd", "missing directory");
	if (argc != 1u) return write_error("cd", "expected one directory");
	return report_change_directory(shell_change_directory(shell, argv[0], strlen(argv[0])));
}

static bool execute_print_directory(const struct shell* shell, size_t argc, char* const argv[]) {
	/* Paths are kept lexically normalized and there are no symbolic links, so -L and -P are equivalent. */
	for (size_t i = 0u; i < argc; i++) {
		if (strcmp(argv[i], "-L") != 0 && strcmp(argv[i], "-P") != 0) return write_error("pwd", "unexpected argument");
	}
	return shell->current_directory_size <= INT32_MAX &&
	       printf("%.*s\n", (int)shell->current_directory_size, shell->current_directory) >= 0;
}

enum echo_option {
	ECHO_OPTION_NO_NEWLINE = 1u << 0,
	ECHO_OPTION_ESCAPES    = 1u << 1,
};

static bool parse_echo_option(const char* argument, uint32_t* options) {
	uint32_t parsed = *options;

	if (argument[0] != '-' || argument[1] == '\0') return false;
	for (const char* letter = argument + 1; *letter != '\0'; letter++) {
		if (*letter == 'n') parsed |= ECHO_OPTION_NO_NEWLINE;
		else if (*letter == 'e') parsed |= ECHO_OPTION_ESCAPES;
		else if (*letter == 'E') parsed &= ~(uint32_t)ECHO_OPTION_ESCAPES;
		else return false;
	}
	*options = parsed;
	return true;
}

static int hex_digit_value(char value) {
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

static bool parse_hex(const char** text, size_t maximum_digits, uint32_t* out_code) {
	size_t digits = 0u;

	int digit;

	*out_code = 0u;
	for (; digits < maximum_digits && (digit = hex_digit_value(**text)) >= 0; digits++, (*text)++)
		*out_code = *out_code * 16u + (uint32_t)digit;
	return digits != 0u;
}

static void append_utf8(char* output, size_t* size, uint32_t code) {
	if (code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) code = 0xfffdu;
	if (code < 0x80u) {
		output[(*size)++] = (char)code;
	}
	else if (code < 0x800u) {
		output[(*size)++] = (char)(0xc0u | code >> 6u);
		output[(*size)++] = (char)(0x80u | (code & 0x3fu));
	}
	else if (code < 0x10000u) {
		output[(*size)++] = (char)(0xe0u | code >> 12u);
		output[(*size)++] = (char)(0x80u | (code >> 6u & 0x3fu));
		output[(*size)++] = (char)(0x80u | (code & 0x3fu));
	}
	else {
		output[(*size)++] = (char)(0xf0u | code >> 18u);
		output[(*size)++] = (char)(0x80u | (code >> 12u & 0x3fu));
		output[(*size)++] = (char)(0x80u | (code >> 6u & 0x3fu));
		output[(*size)++] = (char)(0x80u | (code & 0x3fu));
	}
}

/* Escapes never expand, so the output needs no more room than the argument. Returns true on \c. */
static bool append_escaped(char* output, size_t* size, const char* argument) {
	static const char letters[] = "abeEfnrtv\\";
	static const char values[]  = "\a\b\x1b\x1b\f\n\r\t\v\\";

	while (*argument != '\0') {
		const char* letter;
		char        value = *argument++;
		uint32_t    code  = 0u;

		if (value != '\\' || *argument == '\0') {
			output[(*size)++] = value;
			continue;
		}
		value  = *argument++;
		letter = memchr(letters, value, sizeof(letters) - 1u);
		if (letter != NULL) {
			output[(*size)++] = values[letter - letters];
			continue;
		}
		if (value == 'c') return true;
		if (value == '0') {
			for (int digits = 0; digits < 3 && *argument >= '0' && *argument <= '7'; digits++)
				code = code * 8u + (uint32_t)(*argument++ - '0');
			output[(*size)++] = (char)code;
			continue;
		}
		if (value == 'x' && parse_hex(&argument, 2u, &code)) {
			output[(*size)++] = (char)code;
			continue;
		}
		if ((value == 'u' || value == 'U') && parse_hex(&argument, value == 'u' ? 4u : 8u, &code)) {
			append_utf8(output, size, code);
			continue;
		}
		output[(*size)++] = '\\';
		output[(*size)++] = value;
	}
	return false;
}

static bool execute_echo(size_t argc, char* const argv[]) {
	uint32_t options  = 0u;
	size_t   capacity = 1u;
	size_t   size     = 0u;
	bool     success;
	char*    output;

	for (; argc != 0u && parse_echo_option(*argv, &options); argc--) argv++;
	for (size_t i = 0u; i < argc; i++) capacity += strlen(argv[i]) + 1u;
	output = malloc(capacity);
	if (output == NULL) return write_error("echo", "out of memory");
	for (size_t i = 0u; i < argc; i++) {
		if (i != 0u) output[size++] = ' ';
		if ((options & ECHO_OPTION_ESCAPES) == 0u) {
			size_t length = strlen(argv[i]);
			memcpy(output + size, argv[i], length);
			size += length;
		}
		else if (append_escaped(output, &size, argv[i])) {
			options |= ECHO_OPTION_NO_NEWLINE;
			break;
		}
	}
	if ((options & ECHO_OPTION_NO_NEWLINE) == 0u) output[size++] = '\n';
	success = fwrite(output, 1u, size, stdout) == size;
	free(output);
	return success;
}

static bool execute_program(struct shell* shell, size_t argc, char* const argv[]) {
	size_t    command_size = strlen(argv[0]);
	uintptr_t exit_code;
	switch (shell_run_program(shell, argv[0], command_size, argc - 1u, (const char* const*)(argv + 1u), &exit_code)) {
	case SHELL_PROGRAM_OK:
		return true;
	case SHELL_PROGRAM_NOT_FOUND:
		return write_unknown_command(argv[0]);
	case SHELL_PROGRAM_EXIT_FAILED:
		return write_program_exit(argv[0], exit_code);
	case SHELL_PROGRAM_FAILED:
	default:
		return write_error(argv[0], "unable to launch program");
	}
}

bool shell_execute_line(struct shell* shell, const char* line, size_t size) {
	struct shell_pipeline   pipeline = {0};
	struct shell_command*   command;
	char**                  argv;
	size_t                  argc;
	bool                    success;
	enum shell_parse_result parse_result;

	if (shell == NULL || (line == NULL && size != 0u)) return false;
	parse_result = shell_parse_line(line, size, &pipeline);
	if (parse_result != SHELL_PARSE_OK) {
		const char* message;
		switch (parse_result) {
		case SHELL_PARSE_UNTERMINATED_QUOTE:
			message = "unterminated quoted string";
			break;
		case SHELL_PARSE_TRAILING_ESCAPE:
			message = "trailing escape character";
			break;
		case SHELL_PARSE_MISSING_COMMAND:
			message = "missing command before or after pipe";
			break;
		case SHELL_PARSE_MISSING_REDIRECTION_TARGET:
			message = "missing redirection target";
			break;
		case SHELL_PARSE_UNSUPPORTED_OPERATOR:
			message = "unsupported shell operator";
			break;
		case SHELL_PARSE_NO_MEMORY:
			message = "out of memory";
			break;
		case SHELL_PARSE_INVALID_ARGUMENT:
		case SHELL_PARSE_OK:
		default:
			message = "invalid command line";
			break;
		}
		return write_error(NULL, message);
	}
	if (pipeline.command_count == 0u) {
		shell_pipeline_destroy(&pipeline);
		return true;
	}
	if (pipeline.command_count != 1u || pipeline.commands[0].redirection_count != 0u) {
		shell_pipeline_destroy(&pipeline);
		return write_error(NULL, "redirections and pipes are not supported yet");
	}
	command = &pipeline.commands[0];
	argc    = command->argc;
	argv    = command->argv;
	if (argc == 0u) {
		shell_pipeline_destroy(&pipeline);
		return true;
	}
	if (strcmp(argv[0], "cd") == 0) success = execute_change_directory(shell, argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "pwd") == 0) success = execute_print_directory(shell, argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "echo") == 0) success = execute_echo(argc - 1u, argv + 1u);
	else if (strcmp(argv[0], "exit") == 0) {
		if (argc != 1u) success = write_error("exit", "expected no arguments");
		else {
			shell->exit_requested = true;
			success               = true;
		}
	}
	else if (strcmp(argv[0], "clear") == 0) {
		if (argc != 1u) success = write_error("clear", "expected no arguments");
		else success = printf("\x1b[2J\x1b[3J\x1b[H") >= 0;
	}
	else success = execute_program(shell, argc, argv);
	shell_pipeline_destroy(&pipeline);
	return success;
}
