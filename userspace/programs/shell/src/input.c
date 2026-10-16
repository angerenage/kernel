#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "shell.h"

enum input_state {
	INPUT_STATE_TEXT,
	INPUT_STATE_ESCAPE,
	INPUT_STATE_CSI,
	INPUT_STATE_SS3,
};

static void erase_last_codepoint(char* line, size_t* size) {
	if (*size == 0u) return;
	(*size)--;
	while (*size != 0u && ((uint8_t)line[*size] & 0xc0u) == 0x80u) (*size)--;
}

static void append_history(struct shell* shell, const char* line, size_t size) {
	char* copy;

	if (size == 0u) return;
	copy = malloc(size);
	if (copy == NULL) return;
	memcpy(copy, line, size);
	if (shell->history_count == SHELL_HISTORY_CAPACITY) {
		free(shell->history[0].line);
		memmove(shell->history, shell->history + 1u, (SHELL_HISTORY_CAPACITY - 1u) * sizeof(shell->history[0]));
		shell->history_count--;
	}
	shell->history[shell->history_count++] = (struct shell_history_entry){.line = copy, .size = size};
}

static bool redraw_line(const struct shell* shell, const char* line, size_t size) {
	return shell_write_string("\r\x1b[2K") && shell_write_prompt(shell) && shell_write(line, size);
}

static bool select_history(const struct shell* shell, bool older, size_t* history_index, char* line, size_t capacity,
                           size_t* size, const char* draft, size_t draft_size) {
	if (older) {
		if (*history_index == 0u) return true;
		(*history_index)--;
	}
	else {
		if (*history_index == shell->history_count) return true;
		(*history_index)++;
	}
	if (*history_index == shell->history_count) {
		memcpy(line, draft, draft_size);
		*size = draft_size;
	}
	else {
		const struct shell_history_entry* entry = &shell->history[*history_index];
		if (entry->size > capacity) return false;
		memcpy(line, entry->line, entry->size);
		*size = entry->size;
	}
	return redraw_line(shell, line, *size);
}

enum shell_input_result shell_read_line(struct shell* shell, char* line, size_t capacity, size_t* out_size) {
	char             draft[SHELL_LINE_CAPACITY];
	size_t           draft_size = 0u;
	size_t           history_index;
	size_t           size        = 0u;
	enum input_state state       = INPUT_STATE_TEXT;
	bool             draft_saved = false;
	bool             csi_plain   = true;

	if (shell == NULL || line == NULL || capacity == 0u || capacity > sizeof(draft) || out_size == NULL)
		return SHELL_INPUT_FAILED;
	history_index = shell->history_count;
	*out_size     = 0u;
	for (;;) {
		uint8_t byte;
		ssize_t count = read(0, &byte, 1u);
		if (count != 1) return SHELL_INPUT_FAILED;

		if (state == INPUT_STATE_ESCAPE) {
			if (byte == '[') {
				state     = INPUT_STATE_CSI;
				csi_plain = true;
			}
			else if (byte == 'O') state = INPUT_STATE_SS3;
			else state = INPUT_STATE_TEXT;
			continue;
		}
		if (state == INPUT_STATE_CSI) {
			if (byte >= 0x40u && byte <= 0x7eu) {
				if (csi_plain && (byte == 'A' || byte == 'B')) {
					if (!draft_saved) {
						memcpy(draft, line, size);
						draft_size  = size;
						draft_saved = true;
					}
					if (!select_history(shell, byte == 'A', &history_index, line, capacity, &size, draft, draft_size))
						return SHELL_INPUT_FAILED;
				}
				state = INPUT_STATE_TEXT;
			}
			else csi_plain = false;
			continue;
		}
		if (state == INPUT_STATE_SS3) {
			if (byte == 'A' || byte == 'B') {
				if (!draft_saved) {
					memcpy(draft, line, size);
					draft_size  = size;
					draft_saved = true;
				}
				if (!select_history(shell, byte == 'A', &history_index, line, capacity, &size, draft, draft_size))
					return SHELL_INPUT_FAILED;
			}
			state = INPUT_STATE_TEXT;
			continue;
		}
		if (byte == 0x1bu) {
			state = INPUT_STATE_ESCAPE;
			continue;
		}
		if (byte == '\n' || byte == '\r') {
			if (!shell_write_string("\n")) return SHELL_INPUT_FAILED;
			append_history(shell, line, size);
			*out_size = size;
			return SHELL_INPUT_SUBMITTED;
		}
		if (byte == 0x03u) {
			if (!shell_write_string("^C\n")) return SHELL_INPUT_FAILED;
			return SHELL_INPUT_INTERRUPTED;
		}
		if (byte == '\b' || byte == 0x7fu) {
			if (size != 0u) {
				erase_last_codepoint(line, &size);
				if (!shell_write_string("\b \b")) return SHELL_INPUT_FAILED;
			}
			continue;
		}
		if (byte < 0x20u || size == capacity) continue;
		line[size++] = (char)byte;
		if (!shell_write(&byte, 1u)) return SHELL_INPUT_FAILED;
	}
}
