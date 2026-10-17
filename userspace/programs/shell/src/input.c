#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "editor.h"
#include "shell.h"

enum input_state {
	INPUT_STATE_TEXT,
	INPUT_STATE_ESCAPE,
	INPUT_STATE_CSI,
	INPUT_STATE_SS3,
};

enum character_read_result {
	CHARACTER_READ_OK,
	CHARACTER_READ_INVALID,
	CHARACTER_READ_FAILED,
};

struct history_navigation {
	char   draft[SHELL_LINE_CAPACITY];
	size_t draft_size;
	size_t draft_cursor;
	size_t index;
	bool   draft_saved;
};

static bool read_input_byte(struct shell* shell, uint8_t* out_byte) {
	if (shell == NULL || out_byte == NULL) return false;
	if (shell->pending_input_count != 0u) {
		*out_byte                 = shell->pending_input[shell->pending_input_head];
		shell->pending_input_head = (shell->pending_input_head + 1u) % SHELL_PENDING_INPUT_CAPACITY;
		shell->pending_input_count--;
		return true;
	}
	return read(0, out_byte, 1u) == 1;
}

static enum character_read_result read_character(struct shell* shell, uint8_t first, char character[4],
                                                 size_t* out_size) {
	size_t size;

	character[0] = (char)first;
	if (first < 0x80u) size = 1u;
	else if (first >= 0xc2u && first <= 0xdfu) size = 2u;
	else if (first >= 0xe0u && first <= 0xefu) size = 3u;
	else if (first >= 0xf0u && first <= 0xf4u) size = 4u;
	else return CHARACTER_READ_INVALID;
	for (size_t i = 1u; i < size; i++) {
		uint8_t byte;
		if (!read_input_byte(shell, &byte)) return CHARACTER_READ_FAILED;
		if ((byte & 0xc0u) != 0x80u) return CHARACTER_READ_INVALID;
		character[i] = (char)byte;
	}
	if ((first == 0xe0u && (uint8_t)character[1] < 0xa0u) || (first == 0xedu && (uint8_t)character[1] >= 0xa0u) ||
	    (first == 0xf0u && (uint8_t)character[1] < 0x90u) || (first == 0xf4u && (uint8_t)character[1] >= 0x90u))
		return CHARACTER_READ_INVALID;
	*out_size = size;
	return CHARACTER_READ_OK;
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

static bool select_history(const struct shell* shell, struct shell_line_editor* editor,
                           struct history_navigation* navigation, bool older) {
	if (!navigation->draft_saved) {
		memcpy(navigation->draft, editor->line, editor->size);
		navigation->draft_size   = editor->size;
		navigation->draft_cursor = editor->cursor;
		navigation->draft_saved  = true;
	}
	if (older) {
		if (navigation->index == 0u) return true;
		navigation->index--;
	}
	else {
		if (navigation->index == shell->history_count) return true;
		navigation->index++;
	}
	if (navigation->index == shell->history_count) {
		return shell_line_editor_replace(editor, navigation->draft, navigation->draft_size, navigation->draft_cursor);
	}
	const struct shell_history_entry* entry = &shell->history[navigation->index];
	return shell_line_editor_replace(editor, entry->line, entry->size, entry->size);
}

static bool handle_escape_key(struct shell* shell, struct shell_line_editor* editor,
                              struct history_navigation* navigation, uint8_t key, bool delete_key) {
	switch (key) {
	case 'A':
		return select_history(shell, editor, navigation, true);
	case 'B':
		return select_history(shell, editor, navigation, false);
	case 'C':
		return shell_line_editor_move_right(editor);
	case 'D':
		return shell_line_editor_move_left(editor);
	case '~':
		return !delete_key || shell_line_editor_delete(editor);
	default:
		return true;
	}
}

enum shell_input_result shell_read_line(struct shell* shell, char* line, size_t capacity, size_t* out_size) {
	struct shell_line_editor  editor;
	struct history_navigation navigation;
	enum input_state          state      = INPUT_STATE_TEXT;
	bool                      csi_plain  = true;
	bool                      csi_delete = false;
	enum shell_input_result   input_result;

	if (shell == NULL || line == NULL || capacity == 0u || capacity > SHELL_LINE_CAPACITY || out_size == NULL)
		return SHELL_INPUT_FAILED;
	if (!shell_line_editor_initialize(&editor, shell, line, capacity)) return SHELL_INPUT_FAILED;
	navigation = (struct history_navigation){.index = shell->history_count};
	*out_size  = 0u;
	for (;;) {
		uint8_t byte;
		if (!read_input_byte(shell, &byte)) {
			input_result = SHELL_INPUT_FAILED;
			break;
		}

		if (state == INPUT_STATE_ESCAPE) {
			if (byte == '[') {
				state      = INPUT_STATE_CSI;
				csi_plain  = true;
				csi_delete = false;
			}
			else if (byte == 'O') state = INPUT_STATE_SS3;
			else state = INPUT_STATE_TEXT;
			continue;
		}
		if (state == INPUT_STATE_CSI) {
			if (byte >= 0x40u && byte <= 0x7eu) {
				if ((csi_plain || (csi_delete && byte == '~')) &&
				    !handle_escape_key(shell, &editor, &navigation, byte, csi_delete)) {
					input_result = SHELL_INPUT_FAILED;
					break;
				}
				state = INPUT_STATE_TEXT;
			}
			else {
				csi_delete = csi_plain && byte == '3';
				csi_plain  = false;
			}
			continue;
		}
		if (state == INPUT_STATE_SS3) {
			if (!handle_escape_key(shell, &editor, &navigation, byte, false)) {
				input_result = SHELL_INPUT_FAILED;
				break;
			}
			state = INPUT_STATE_TEXT;
			continue;
		}
		if (byte == 0x1bu) {
			state = INPUT_STATE_ESCAPE;
			continue;
		}
		if (byte == '\n' || byte == '\r') {
			if (!shell_line_editor_finish(&editor)) {
				input_result = SHELL_INPUT_FAILED;
				break;
			}
			append_history(shell, editor.line, editor.size);
			*out_size    = editor.size;
			input_result = SHELL_INPUT_SUBMITTED;
			break;
		}
		if (byte == 0x03u) {
			input_result = shell_line_editor_interrupt(&editor) ? SHELL_INPUT_INTERRUPTED : SHELL_INPUT_FAILED;
			break;
		}
		if (byte == '\b' || byte == 0x7fu) {
			if (!shell_line_editor_backspace(&editor)) {
				input_result = SHELL_INPUT_FAILED;
				break;
			}
			continue;
		}
		if (byte < 0x20u) continue;
		char                       character[4];
		size_t                     character_size;
		enum character_read_result result = read_character(shell, byte, character, &character_size);
		if (result == CHARACTER_READ_FAILED) {
			input_result = SHELL_INPUT_FAILED;
			break;
		}
		if (result == CHARACTER_READ_INVALID || character_size > editor.capacity - editor.size) continue;
		if (!shell_line_editor_insert(&editor, character, character_size)) {
			input_result = SHELL_INPUT_FAILED;
			break;
		}
	}
	shell_line_editor_destroy(&editor);
	return input_result;
}
