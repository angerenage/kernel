#include <boot/info.h>
#include <kernel/cmdline.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool cmdline_is_space(char ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static bool cmdline_name_equals(const char* token, size_t token_len, const char* name) {
	size_t name_len;
	size_t token_name_len = 0u;

	if (token == NULL || name == NULL) return false;
	while (token_name_len < token_len && token[token_name_len] != '=') token_name_len++;
	name_len = strlen(name);
	return token_name_len == name_len && memcmp(token, name, name_len) == 0;
}

static bool cmdline_token_recognized(const char* token, size_t token_len) {
	static const char* const recognized[] = {
		"debug",
		"kernel.debug",
		"loglevel",
		"selftest",
		"selftests",
		"kernel.selftest",
		"kernel.selftests",
		"selftest.suite",
		"kernel.selftest.suite",
		"kernel.selftest.iommu",
	};

	for (size_t index = 0u; index < sizeof(recognized) / sizeof(recognized[0]); index++)
		if (cmdline_name_equals(token, token_len, recognized[index])) return true;
	return false;
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

	cmdline = boot_cmdline_current();
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

bool kernel_cmdline_userspace_arguments(char* buffer, size_t capacity, size_t* out_argc, size_t* out_size) {
	const char* cmdline;
	const char* cursor;
	size_t      argc = 0u;
	size_t      size = 0u;

	if (out_argc == NULL || out_size == NULL || (buffer == NULL && capacity != 0u)) return false;
	*out_argc = 0u;
	*out_size = 0u;
	cmdline   = boot_cmdline_current();
	if (cmdline == NULL) return true;
	cursor = cmdline;
	while (*cursor != '\0') {
		const char* token;
		size_t      token_size;

		while (cmdline_is_space(*cursor)) cursor++;
		if (*cursor == '\0') break;
		token = cursor;
		while (*cursor != '\0' && !cmdline_is_space(*cursor)) cursor++;
		token_size = (size_t)(cursor - token);
		if (cmdline_token_recognized(token, token_size)) continue;
		if (argc == SIZE_MAX || token_size == SIZE_MAX || size > SIZE_MAX - token_size - 1u) return false;
		if (buffer != NULL) {
			if (size > capacity || token_size + 1u > capacity - size) return false;
			memcpy(buffer + size, token, token_size);
			buffer[size + token_size] = '\0';
		}
		size += token_size + 1u;
		argc++;
	}
	*out_argc = argc;
	*out_size = size;
	return true;
}
