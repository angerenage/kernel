#pragma once

#include <stddef.h>

/* The order of redirections is preserved (later redirections may override earlier ones). */
enum shell_redirection_kind {
	SHELL_REDIRECT_INPUT,
	SHELL_REDIRECT_OUTPUT,
	SHELL_REDIRECT_APPEND,
};

struct shell_redirection {
	enum shell_redirection_kind kind;
	const char*                 path;
};

struct shell_command {
	char**                    argv; /* NULL-terminated; strings owned by shell_pipeline.storage. */
	size_t                    argc;
	struct shell_redirection* redirections;
	size_t                    redirection_count;
};

struct shell_pipeline {
	struct shell_command* commands;
	size_t                command_count;
	char*                 storage;
};

enum shell_parse_result {
	SHELL_PARSE_OK,
	SHELL_PARSE_INVALID_ARGUMENT,
	SHELL_PARSE_NO_MEMORY,
	SHELL_PARSE_UNTERMINATED_QUOTE,
	SHELL_PARSE_TRAILING_ESCAPE,
	SHELL_PARSE_MISSING_COMMAND,
	SHELL_PARSE_MISSING_REDIRECTION_TARGET,
	SHELL_PARSE_UNSUPPORTED_OPERATOR,
};

/* Parse a command line into a pipeline of commands. */
enum shell_parse_result shell_parse_line(const char* line, size_t size, struct shell_pipeline* out_pipeline);

/* Free the memory allocated for a pipeline. */
void shell_pipeline_destroy(struct shell_pipeline* pipeline);
