#include "editor.h"

#include <libc/stdlib.h>
#include <libc/string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shell.h"

#define SHELL_QUOTE_COLOR "\x1b[33m"
#define SHELL_DEFAULT_COLOR "\x1b[0m"

struct quote_state {
	char quote;
	bool escaped;
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

static size_t line_position(const struct shell_line_editor* editor, size_t offset) {
	return shell_prompt_width(editor->shell) + count_codepoints(editor->line, offset);
}

static bool reserve_render(struct shell_line_editor* editor, size_t additional) {
	size_t required;
	size_t capacity;
	char*  resized;

	if (additional > SIZE_MAX - editor->render_size) return false;
	required = editor->render_size + additional;
	if (required <= editor->render_capacity) return true;
	capacity = editor->render_capacity == 0u ? 256u : editor->render_capacity;
	while (capacity < required) {
		if (capacity > SIZE_MAX / 2u) {
			capacity = required;
			break;
		}
		capacity *= 2u;
	}
	resized = realloc(editor->render_data, capacity);
	if (resized == NULL) return false;
	editor->render_data     = resized;
	editor->render_capacity = capacity;
	return true;
}

static bool append_bytes(struct shell_line_editor* editor, const void* data, size_t size) {
	if (size != 0u && data == NULL) return false;
	if (!reserve_render(editor, size)) return false;
	memcpy(editor->render_data + editor->render_size, data, size);
	editor->render_size += size;
	return true;
}

static bool append_string(struct shell_line_editor* editor, const char* string) {
	return append_bytes(editor, string, strlen(string));
}

static bool append_csi(struct shell_line_editor* editor, size_t value, char final) {
	char   sequence[3u + sizeof(size_t) * 3u];
	size_t end = sizeof(sequence);
	size_t i   = end;

	if (value == 0u) return true;
	sequence[--i] = final;
	do {
		sequence[--i] = (char)('0' + value % 10u);
		value /= 10u;
	} while (value != 0u);
	sequence[--i] = '[';
	sequence[--i] = '\x1b';
	return append_bytes(editor, sequence + i, end - i);
}

static bool append_cursor_move(struct shell_line_editor* editor, size_t from, size_t to) {
	size_t columns = editor->shell->terminal_columns;
	size_t from_row;
	size_t to_row;
	size_t from_column;
	size_t to_column;

	if (columns == 0u) return false;
	from_row    = from / columns;
	to_row      = to / columns;
	from_column = from % columns;
	to_column   = to % columns;
	if (to_row < from_row && !append_csi(editor, from_row - to_row, 'A')) return false;
	if (to_row > from_row && !append_csi(editor, to_row - from_row, 'B')) return false;
	return from_column == to_column || append_csi(editor, to_column + 1u, 'G');
}

static bool append_normalized_wrap(struct shell_line_editor* editor, size_t position) {
	return position == 0u || position % editor->shell->terminal_columns != 0u || append_string(editor, "\n");
}

static void advance_quote_state(struct quote_state* state, char value) {
	if (state->escaped) {
		state->escaped = false;
		return;
	}
	if (value == '\\') {
		state->escaped = true;
		return;
	}
	if (state->quote == '\0' && (value == '\'' || value == '"')) state->quote = value;
	else if (value == state->quote) state->quote = '\0';
}

static struct quote_state quote_state_before(const struct shell_line_editor* editor, size_t offset) {
	struct quote_state state = {0};

	for (size_t index = 0u; index < offset; index++) advance_quote_state(&state, editor->line[index]);
	return state;
}

static bool append_colored_line(struct shell_line_editor* editor, size_t start) {
	struct quote_state state  = quote_state_before(editor, start);
	bool               yellow = false;

	for (size_t offset = start; offset < editor->size; offset++) {
		bool next_yellow =
			state.quote != '\0' || (!state.escaped && (editor->line[offset] == '\'' || editor->line[offset] == '"'));
		if (next_yellow != yellow && !append_string(editor, next_yellow ? SHELL_QUOTE_COLOR : SHELL_DEFAULT_COLOR))
			return false;
		yellow = next_yellow;
		if (!append_bytes(editor, editor->line + offset, 1u)) return false;
		advance_quote_state(&state, editor->line[offset]);
	}
	return !yellow || append_string(editor, SHELL_DEFAULT_COLOR);
}

static bool write_render(struct shell_line_editor* editor) {
	bool success        = editor->render_size == 0u ||
	                      fwrite(editor->render_data, 1u, editor->render_size, stdout) == editor->render_size;
	editor->render_size = 0u;
	return success;
}

static bool append_spaces(struct shell_line_editor* editor, size_t count) {
	static const char spaces[] = "                                                                ";

	while (count != 0u) {
		size_t chunk = count < sizeof(spaces) - 1u ? count : sizeof(spaces) - 1u;
		if (!append_bytes(editor, spaces, chunk)) return false;
		count -= chunk;
	}
	return true;
}

static bool refresh_suffix(struct shell_line_editor* editor, size_t start, size_t old_cursor_position,
                           size_t old_end_position) {
	size_t start_position   = line_position(editor, start);
	size_t end_position     = line_position(editor, editor->size);
	size_t cursor_position  = line_position(editor, editor->cursor);
	size_t current_position = end_position;

	editor->render_size = 0u;
	if (!append_cursor_move(editor, old_cursor_position, start_position) || !append_colored_line(editor, start) ||
	    !append_normalized_wrap(editor, end_position))
		return false;
	if (old_end_position > end_position) {
		if (!append_spaces(editor, old_end_position - end_position) ||
		    !append_normalized_wrap(editor, old_end_position))
			return false;
		current_position = old_end_position;
	}
	if (!append_cursor_move(editor, current_position, cursor_position)) return false;
	return write_render(editor);
}

static bool move_cursor(struct shell_line_editor* editor, size_t next) {
	size_t from = line_position(editor, editor->cursor);
	size_t to   = line_position(editor, next);

	editor->render_size = 0u;
	if (!append_cursor_move(editor, from, to) || !write_render(editor)) return false;
	editor->cursor = next;
	return true;
}

bool shell_line_editor_initialize(struct shell_line_editor* editor, const struct shell* shell, char* line,
                                  size_t capacity) {
	if (editor == NULL || shell == NULL || shell->terminal_columns == 0u || line == NULL || capacity == 0u)
		return false;
	*editor = (struct shell_line_editor){
		.shell    = shell,
		.line     = line,
		.capacity = capacity,
	};
	return true;
}

void shell_line_editor_destroy(struct shell_line_editor* editor) {
	if (editor == NULL) return;
	free(editor->render_data);
	editor->render_data     = NULL;
	editor->render_size     = 0u;
	editor->render_capacity = 0u;
}

bool shell_line_editor_insert(struct shell_line_editor* editor, const char* character, size_t size) {
	size_t old_cursor_position;
	size_t old_end_position;
	size_t insertion;

	if (editor == NULL || character == NULL || size == 0u || size > editor->capacity - editor->size) return false;
	old_cursor_position = line_position(editor, editor->cursor);
	old_end_position    = line_position(editor, editor->size);
	insertion           = editor->cursor;
	memmove(editor->line + insertion + size, editor->line + insertion, editor->size - insertion);
	memcpy(editor->line + insertion, character, size);
	editor->cursor += size;
	editor->size += size;
	return refresh_suffix(editor, insertion, old_cursor_position, old_end_position);
}

bool shell_line_editor_backspace(struct shell_line_editor* editor) {
	size_t old_cursor_position;
	size_t old_end_position;
	size_t previous;

	if (editor == NULL) return false;
	if (editor->cursor == 0u) return true;
	old_cursor_position = line_position(editor, editor->cursor);
	old_end_position    = line_position(editor, editor->size);
	previous            = previous_codepoint(editor->line, editor->cursor);
	memmove(editor->line + previous, editor->line + editor->cursor, editor->size - editor->cursor);
	editor->size -= editor->cursor - previous;
	editor->cursor = previous;
	return refresh_suffix(editor, previous, old_cursor_position, old_end_position);
}

bool shell_line_editor_delete(struct shell_line_editor* editor) {
	size_t old_cursor_position;
	size_t old_end_position;
	size_t next;

	if (editor == NULL) return false;
	if (editor->cursor == editor->size) return true;
	old_cursor_position = line_position(editor, editor->cursor);
	old_end_position    = line_position(editor, editor->size);
	next                = next_codepoint(editor->line, editor->size, editor->cursor);
	memmove(editor->line + editor->cursor, editor->line + next, editor->size - next);
	editor->size -= next - editor->cursor;
	return refresh_suffix(editor, editor->cursor, old_cursor_position, old_end_position);
}

bool shell_line_editor_move_left(struct shell_line_editor* editor) {
	if (editor == NULL) return false;
	return editor->cursor == 0u || move_cursor(editor, previous_codepoint(editor->line, editor->cursor));
}

bool shell_line_editor_move_right(struct shell_line_editor* editor) {
	if (editor == NULL) return false;
	return editor->cursor == editor->size ||
	       move_cursor(editor, next_codepoint(editor->line, editor->size, editor->cursor));
}

bool shell_line_editor_replace(struct shell_line_editor* editor, const char* line, size_t size, size_t cursor) {
	size_t old_cursor_position;
	size_t old_end_position;

	if (editor == NULL || (size != 0u && line == NULL) || size > editor->capacity || cursor > size) return false;
	old_cursor_position = line_position(editor, editor->cursor);
	old_end_position    = line_position(editor, editor->size);
	memmove(editor->line, line, size);
	editor->size   = size;
	editor->cursor = cursor;
	return refresh_suffix(editor, 0u, old_cursor_position, old_end_position);
}

bool shell_line_editor_finish(struct shell_line_editor* editor) {
	size_t end_position;

	if (editor == NULL) return false;
	end_position        = line_position(editor, editor->size);
	editor->render_size = 0u;
	if (!append_cursor_move(editor, line_position(editor, editor->cursor), end_position) ||
	    (end_position % editor->shell->terminal_columns != 0u && !append_string(editor, "\n")))
		return false;
	if (!write_render(editor)) return false;
	editor->cursor = editor->size;
	return true;
}

bool shell_line_editor_interrupt(struct shell_line_editor* editor) {
	if (!shell_line_editor_finish(editor)) return false;
	return printf("^C\n") >= 0;
}
