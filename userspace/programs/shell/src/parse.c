#include "parse.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum token_kind {
	TOKEN_END,
	TOKEN_WORD,
	TOKEN_PIPE,
	TOKEN_INPUT,
	TOKEN_OUTPUT,
	TOKEN_APPEND,
};

struct token {
	enum token_kind kind;
	char*           text;
};

struct lexer {
	const char* input;
	size_t      size;
	size_t      index;
	char*       storage;
	size_t      used;
};

static bool is_space(char value) {
	return value == ' ' || value == '\t';
}

static bool is_operator(char value) {
	return value == '<' || value == '>' || value == '|' || value == '&' || value == ';';
}

static enum shell_parse_result next_token(struct lexer* lexer, struct token* out) {
	char   quote   = '\0';
	bool   started = false;
	size_t start;

	while (lexer->index < lexer->size && is_space(lexer->input[lexer->index])) lexer->index++;
	*out = (struct token){.kind = TOKEN_END};
	if (lexer->index == lexer->size) return SHELL_PARSE_OK;

	char value = lexer->input[lexer->index];
	if (value == '&' || value == ';') return SHELL_PARSE_UNSUPPORTED_OPERATOR;
	if (value == '|' || value == '<' || value == '>') {
		lexer->index++;
		if (value == '|') {
			if (lexer->index < lexer->size && (lexer->input[lexer->index] == '|' || lexer->input[lexer->index] == '&'))
				return SHELL_PARSE_UNSUPPORTED_OPERATOR;
			out->kind = TOKEN_PIPE;
		}
		else if (value == '<') {
			if (lexer->index < lexer->size && (lexer->input[lexer->index] == '<' || lexer->input[lexer->index] == '&'))
				return SHELL_PARSE_UNSUPPORTED_OPERATOR;
			out->kind = TOKEN_INPUT;
		}
		else if (lexer->index < lexer->size && lexer->input[lexer->index] == '>') {
			lexer->index++;
			out->kind = TOKEN_APPEND;
		}
		else {
			if (lexer->index < lexer->size && lexer->input[lexer->index] == '&')
				return SHELL_PARSE_UNSUPPORTED_OPERATOR;
			out->kind = TOKEN_OUTPUT;
		}
		return SHELL_PARSE_OK;
	}

	start = lexer->used;
	while (lexer->index < lexer->size) {
		value = lexer->input[lexer->index++];
		if (value == '\0') return SHELL_PARSE_INVALID_ARGUMENT;
		if (value == '\\' && quote != '\'') {
			if (lexer->index == lexer->size) return SHELL_PARSE_TRAILING_ESCAPE;
			char escaped = lexer->input[lexer->index++];
			if (escaped == '\0') return SHELL_PARSE_INVALID_ARGUMENT;
			if (quote == '"' && memchr("\"\\$`", escaped, 4u) == NULL) lexer->storage[lexer->used++] = '\\';
			lexer->storage[lexer->used++] = escaped;
			started                       = true;
		}
		else if (quote != '\0') {
			if (value == quote) quote = '\0';
			else lexer->storage[lexer->used++] = value;
		}
		else if (value == '\'' || value == '"') {
			quote   = value;
			started = true;
		}
		else if (is_space(value) || is_operator(value)) {
			lexer->index--;
			break;
		}
		else {
			lexer->storage[lexer->used++] = value;
			started                       = true;
		}
	}
	if (quote != '\0') return SHELL_PARSE_UNTERMINATED_QUOTE;
	if (!started) return SHELL_PARSE_INVALID_ARGUMENT;
	lexer->storage[lexer->used++] = '\0';
	*out                          = (struct token){.kind = TOKEN_WORD, .text = lexer->storage + start};
	return SHELL_PARSE_OK;
}

void shell_pipeline_destroy(struct shell_pipeline* pipeline) {
	if (pipeline == NULL) return;
	for (size_t i = 0u; i < pipeline->command_count; i++) {
		free(pipeline->commands[i].argv);
		free(pipeline->commands[i].redirections);
	}
	free(pipeline->commands);
	free(pipeline->storage);
	*pipeline = (struct shell_pipeline){0};
}

static bool add_command(struct shell_pipeline* pipeline) {
	struct shell_command* grown;

	if (pipeline->command_count >= SIZE_MAX / sizeof(*pipeline->commands)) return false;
	grown = realloc(pipeline->commands, (pipeline->command_count + 1u) * sizeof(*pipeline->commands));
	if (grown == NULL) return false;
	pipeline->commands                            = grown;
	pipeline->commands[pipeline->command_count++] = (struct shell_command){0};
	return true;
}

static bool add_argument(struct shell_command* command, char* argument) {
	char** grown;

	if (command->argc > SIZE_MAX / sizeof(*command->argv) - 2u) return false;
	grown = realloc(command->argv, (command->argc + 2u) * sizeof(*command->argv));
	if (grown == NULL) return false;
	command->argv                  = grown;
	command->argv[command->argc++] = argument;
	command->argv[command->argc]   = NULL;
	return true;
}

static bool add_redirection(struct shell_command* command, enum shell_redirection_kind kind, char* path) {
	struct shell_redirection* grown;

	if (command->redirection_count >= SIZE_MAX / sizeof(*command->redirections)) return false;
	grown = realloc(command->redirections, (command->redirection_count + 1u) * sizeof(*command->redirections));
	if (grown == NULL) return false;
	command->redirections                               = grown;
	command->redirections[command->redirection_count++] = (struct shell_redirection){.kind = kind, .path = path};
	return true;
}

enum shell_parse_result shell_parse_line(const char* line, size_t size, struct shell_pipeline* out_pipeline) {
	struct shell_pipeline   pipeline = {0};
	struct lexer            lexer;
	struct token            token;
	enum shell_parse_result result;
	bool                    saw_token = false;

	if (out_pipeline == NULL) return SHELL_PARSE_INVALID_ARGUMENT;
	*out_pipeline = (struct shell_pipeline){0};
	if (size == SIZE_MAX || (line == NULL && size != 0u)) return SHELL_PARSE_INVALID_ARGUMENT;
	if (size == 0u) return SHELL_PARSE_OK;
	pipeline.storage = malloc(size + 1u);
	if (pipeline.storage == NULL) return SHELL_PARSE_NO_MEMORY;
	lexer = (struct lexer){.input = line, .size = size, .storage = pipeline.storage};

	for (;;) {
		struct shell_command* command;
		result = next_token(&lexer, &token);
		if (result != SHELL_PARSE_OK) goto fail;
		if (token.kind == TOKEN_END) break;
		if (!saw_token) {
			if (!add_command(&pipeline)) {
				result = SHELL_PARSE_NO_MEMORY;
				goto fail;
			}
			saw_token = true;
		}
		command = &pipeline.commands[pipeline.command_count - 1u];
		if (token.kind == TOKEN_WORD) {
			if (!add_argument(command, token.text)) {
				result = SHELL_PARSE_NO_MEMORY;
				goto fail;
			}
		}
		else if (token.kind == TOKEN_PIPE) {
			if (command->argc == 0u) {
				result = SHELL_PARSE_MISSING_COMMAND;
				goto fail;
			}
			if (!add_command(&pipeline)) {
				result = SHELL_PARSE_NO_MEMORY;
				goto fail;
			}
		}
		else {
			enum shell_redirection_kind kind = token.kind == TOKEN_INPUT    ? SHELL_REDIRECT_INPUT
			                                   : token.kind == TOKEN_APPEND ? SHELL_REDIRECT_APPEND
			                                                                : SHELL_REDIRECT_OUTPUT;
			result                           = next_token(&lexer, &token);
			if (result != SHELL_PARSE_OK) goto fail;
			if (token.kind != TOKEN_WORD) {
				result = SHELL_PARSE_MISSING_REDIRECTION_TARGET;
				goto fail;
			}
			if (!add_redirection(command, kind, token.text)) {
				result = SHELL_PARSE_NO_MEMORY;
				goto fail;
			}
		}
	}
	if (pipeline.command_count > 1u && pipeline.commands[pipeline.command_count - 1u].argc == 0u) {
		result = SHELL_PARSE_MISSING_COMMAND;
		goto fail;
	}
	*out_pipeline = pipeline;
	return SHELL_PARSE_OK;
fail:
	shell_pipeline_destroy(&pipeline);
	return result;
}
