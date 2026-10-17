#include <base/cap.h>
#include <stddef.h>

#include "shell.h"

int main(int argc, char** argv, size_t capc, const cap_id_t* capv) {
	struct shell shell;
	char         line[SHELL_LINE_CAPACITY];

	(void)argv;
	if (argc != 0 || capc != 0u || capv != NULL) return 1;
	if (!shell_initialize(&shell)) return 1;
	for (;;) {
		size_t                  line_size;
		enum shell_input_result input_result;

		if (!shell_write_prompt(&shell)) goto cleanup;
		input_result = shell_read_line(&shell, line, sizeof(line), &line_size);
		if (input_result == SHELL_INPUT_FAILED) goto cleanup;
		if (input_result == SHELL_INPUT_INTERRUPTED) continue;
		if (!shell_execute_line(&shell, line, line_size)) goto cleanup;
	}

cleanup:
	shell_destroy(&shell);
	return 1;
}
