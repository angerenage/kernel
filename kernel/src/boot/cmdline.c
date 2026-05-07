#include <kernel/boot.h>
#include <kernel/cmdline.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static bool cmdline_is_space(char ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

bool kernel_cmdline_value_equals(const char* value, size_t value_len, const char* expected) {
	size_t expected_len;

	if (value == NULL || expected == NULL) return false;

	expected_len = strlen(expected);
	return value_len == expected_len && memcmp(value, expected, value_len) == 0;
}

static bool cmdline_token_read_option_value(const char* token, size_t token_len, const char* name, const char** value,
                                            size_t* value_len) {
	size_t name_len;

	if (token == NULL || name == NULL || value == NULL || value_len == NULL) return false;

	name_len = strlen(name);
	if (token_len <= name_len + 1u || memcmp(token, name, name_len) != 0 || token[name_len] != '=') return false;

	*value     = token + name_len + 1u;
	*value_len = token_len - name_len - 1u;
	return *value_len > 0u;
}

bool kernel_cmdline_option_enabled(const char* name) {
	const char* value;
	size_t      value_len;

	if (!kernel_cmdline_option_value(name, &value, &value_len)) return false;
	if (value == NULL) return true;

	return kernel_cmdline_value_equals(value, value_len, "1") ||
	       kernel_cmdline_value_equals(value, value_len, "true") ||
	       kernel_cmdline_value_equals(value, value_len, "yes") || kernel_cmdline_value_equals(value, value_len, "on");
}

bool kernel_cmdline_option_value(const char* name, const char** value, size_t* value_len) {
	const char* cmdline;
	const char* cursor;

	if (name == NULL || value == NULL || value_len == NULL) return false;

	*value     = NULL;
	*value_len = 0u;

	cmdline = kernel_boot_cmdline();
	if (cmdline == NULL) return false;

	cursor = cmdline;
	while (*cursor != '\0') {
		const char* token_start;
		size_t      token_len;

		while (cmdline_is_space(*cursor)) cursor++;
		if (*cursor == '\0') break;

		token_start = cursor;
		while (*cursor != '\0' && !cmdline_is_space(*cursor)) cursor++;
		token_len = (size_t)(cursor - token_start);

		if (kernel_cmdline_value_equals(token_start, token_len, name)) return true;
		if (cmdline_token_read_option_value(token_start, token_len, name, value, value_len)) return true;
	}

	return false;
}
