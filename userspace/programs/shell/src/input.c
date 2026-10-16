#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "shell.h"

#define SHELL_QUOTE_COLOR "\x1b[33m"
#define SHELL_DEFAULT_COLOR "\x1b[0m"

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

static size_t previous_codepoint(const char* line, size_t cursor) {
	if (cursor == 0u) return 0u;
	cursor--;
	while (cursor != 0u && ((uint8_t)line[cursor] & 0xc0u) == 0x80u) cursor--;
	return cursor;
}

static size_t next_codepoint(const char* line, size_t size, size_t cursor) {
	if (cursor == size) return size;
	cursor++;
	while (cursor != size && ((uint8_t)line[cursor] & 0xc0u) == 0x80u) cursor++;
	return cursor;
}

static size_t count_codepoints(const char* text, size_t size) {
	size_t count = 0u;
	for (size_t i = 0u; i < size; i++) {
		if (((uint8_t)text[i] & 0xc0u) != 0x80u) count++;
	}
	return count;
}

static char quote_before(const char* line, size_t offset) {
	char quote = '\0';

	for (size_t i = 0u; i < offset; i++) {
		if (quote == '\0' && (line[i] == '\'' || line[i] == '"')) quote = line[i];
		else if (line[i] == quote) quote = '\0';
	}
	return quote;
}

static bool write_colored_line(const char* line, size_t size, size_t start) {
	char   quote  = quote_before(line, start);
	size_t offset = start;

	while (offset < size) {
		size_t begin = offset;
		if (quote == '\0') {
			while (offset < size && line[offset] != '\'' && line[offset] != '"') offset++;
			if (offset != begin && !shell_write(line + begin, offset - begin)) return false;
			if (offset == size) break;
			quote = line[offset++];
			begin = offset - 1u;
		}
		while (offset < size) {
			if (line[offset++] == quote) {
				quote = '\0';
				break;
			}
		}
		if (!shell_write_string(SHELL_QUOTE_COLOR) || !shell_write(line + begin, offset - begin) ||
		    !shell_write_string(SHELL_DEFAULT_COLOR))
			return false;
	}
	return true;
}

static enum character_read_result read_character(uint8_t first, char character[4], size_t* out_size) {
	size_t size;

	character[0] = (char)first;
	if (first < 0x80u) size = 1u;
	else if (first >= 0xc2u && first <= 0xdfu) size = 2u;
	else if (first >= 0xe0u && first <= 0xefu) size = 3u;
	else if (first >= 0xf0u && first <= 0xf4u) size = 4u;
	else return CHARACTER_READ_INVALID;
	for (size_t i = 1u; i < size; i++) {
		uint8_t byte;
		if (read(0, &byte, 1u) != 1) return CHARACTER_READ_FAILED;
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

static bool move_cursor_left(size_t count) {
	char   sequence[3u + sizeof(size_t) * 3u];
	size_t end = sizeof(sequence);
	size_t i   = end;

	if (count == 0u) return true;
	sequence[--i] = 'D';
	do {
		sequence[--i] = (char)('0' + count % 10u);
		count /= 10u;
	} while (count != 0u);
	sequence[--i] = '[';
	sequence[--i] = '\x1b';
	return shell_write(sequence + i, end - i);
}

static bool redraw_line(const struct shell* shell, const char* line, size_t size, size_t cursor) {
	return shell_write_string("\r\x1b[2K") && shell_write_prompt(shell) && write_colored_line(line, size, 0u) &&
	       move_cursor_left(count_codepoints(line + cursor, size - cursor));
}

static bool erase_before_cursor(char* line, size_t* size, size_t* cursor) {
	size_t old_cursor = *cursor;
	size_t previous   = previous_codepoint(line, old_cursor);
	size_t tail_size  = *size - old_cursor;

	memmove(line + previous, line + old_cursor, tail_size);
	*size -= old_cursor - previous;
	*cursor = previous;
	if (tail_size == 0u) return shell_write_string("\b \b");
	return shell_write_string("\x1b[D") && write_colored_line(line, *size, previous) && shell_write_string(" ") &&
	       move_cursor_left(count_codepoints(line + previous, *size - previous) + 1u);
}

static bool erase_after_cursor(char* line, size_t* size, size_t cursor) {
	size_t next      = next_codepoint(line, *size, cursor);
	size_t tail_size = *size - next;

	memmove(line + cursor, line + next, tail_size);
	*size -= next - cursor;
	return write_colored_line(line, *size, cursor) && shell_write_string(" ") &&
	       move_cursor_left(count_codepoints(line + cursor, tail_size) + 1u);
}

static bool insert_character(char* line, size_t* size, size_t* cursor, const char* character, size_t character_size) {
	size_t insertion = *cursor;
	size_t tail_size = *size - insertion;

	memmove(line + insertion + character_size, line + insertion, tail_size);
	memcpy(line + insertion, character, character_size);
	*cursor += character_size;
	*size += character_size;
	return write_colored_line(line, *size, insertion) &&
	       move_cursor_left(count_codepoints(line + *cursor, *size - *cursor));
}

static bool select_history(const struct shell* shell, bool older, size_t* history_index, char* line, size_t capacity,
                           size_t* size, size_t* cursor, const char* draft, size_t draft_size, size_t draft_cursor) {
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
		*size   = draft_size;
		*cursor = draft_cursor;
	}
	else {
		const struct shell_history_entry* entry = &shell->history[*history_index];
		if (entry->size > capacity) return false;
		memcpy(line, entry->line, entry->size);
		*size   = entry->size;
		*cursor = entry->size;
	}
	return redraw_line(shell, line, *size, *cursor);
}

enum shell_input_result shell_read_line(struct shell* shell, char* line, size_t capacity, size_t* out_size) {
	char             draft[SHELL_LINE_CAPACITY];
	size_t           draft_size   = 0u;
	size_t           draft_cursor = 0u;
	size_t           history_index;
	size_t           size        = 0u;
	size_t           cursor      = 0u;
	enum input_state state       = INPUT_STATE_TEXT;
	bool             draft_saved = false;
	bool             csi_plain   = true;
	bool             csi_delete  = false;

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
				if (csi_plain && (byte == 'A' || byte == 'B')) {
					if (!draft_saved) {
						memcpy(draft, line, size);
						draft_size   = size;
						draft_cursor = cursor;
						draft_saved  = true;
					}
					if (!select_history(shell,
					                    byte == 'A',
					                    &history_index,
					                    line,
					                    capacity,
					                    &size,
					                    &cursor,
					                    draft,
					                    draft_size,
					                    draft_cursor))
						return SHELL_INPUT_FAILED;
				}
				else if (csi_plain && byte == 'D' && cursor != 0u) {
					cursor = previous_codepoint(line, cursor);
					if (!shell_write_string("\x1b[D")) return SHELL_INPUT_FAILED;
				}
				else if (csi_plain && byte == 'C' && cursor != size) {
					cursor = next_codepoint(line, size, cursor);
					if (!shell_write_string("\x1b[C")) return SHELL_INPUT_FAILED;
				}
				else if (csi_delete && byte == '~' && cursor != size) {
					if (!erase_after_cursor(line, &size, cursor)) return SHELL_INPUT_FAILED;
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
			if (byte == 'A' || byte == 'B') {
				if (!draft_saved) {
					memcpy(draft, line, size);
					draft_size   = size;
					draft_cursor = cursor;
					draft_saved  = true;
				}
				if (!select_history(shell,
				                    byte == 'A',
				                    &history_index,
				                    line,
				                    capacity,
				                    &size,
				                    &cursor,
				                    draft,
				                    draft_size,
				                    draft_cursor))
					return SHELL_INPUT_FAILED;
			}
			else if (byte == 'D' && cursor != 0u) {
				cursor = previous_codepoint(line, cursor);
				if (!shell_write_string("\x1b[D")) return SHELL_INPUT_FAILED;
			}
			else if (byte == 'C' && cursor != size) {
				cursor = next_codepoint(line, size, cursor);
				if (!shell_write_string("\x1b[C")) return SHELL_INPUT_FAILED;
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
			if (cursor != 0u) {
				if (!erase_before_cursor(line, &size, &cursor)) return SHELL_INPUT_FAILED;
			}
			continue;
		}
		if (byte < 0x20u) continue;
		char                       character[4];
		size_t                     character_size;
		enum character_read_result result = read_character(byte, character, &character_size);
		if (result == CHARACTER_READ_FAILED) return SHELL_INPUT_FAILED;
		if (result == CHARACTER_READ_INVALID || character_size > capacity - size) continue;
		if (!insert_character(line, &size, &cursor, character, character_size)) return SHELL_INPUT_FAILED;
	}
}
