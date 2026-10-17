#pragma once

#include <stdbool.h>
#include <stddef.h>

struct shell;

/* Screen and buffer state for one shell command line. */
struct shell_line_editor {
	const struct shell* shell;
	char*               line;
	size_t              capacity;
	size_t              size;
	size_t              cursor;
	char*               render_data;
	size_t              render_size;
	size_t              render_capacity;
};

/* Initialize an editor after its prompt has been rendered. */
bool shell_line_editor_initialize(struct shell_line_editor* editor, const struct shell* shell, char* line,
                                  size_t capacity);

/* Release temporary rendering storage retained by an editor. */
void shell_line_editor_destroy(struct shell_line_editor* editor);

/* Insert one complete UTF-8 code point at the cursor. */
bool shell_line_editor_insert(struct shell_line_editor* editor, const char* character, size_t size);

/* Delete the code point immediately before the cursor. */
bool shell_line_editor_backspace(struct shell_line_editor* editor);

/* Delete the code point at the cursor. */
bool shell_line_editor_delete(struct shell_line_editor* editor);

/* Move the cursor by one UTF-8 code point when possible. */
bool shell_line_editor_move_left(struct shell_line_editor* editor);
bool shell_line_editor_move_right(struct shell_line_editor* editor);

/* Replace the edited line and repaint its complete visual extent. */
bool shell_line_editor_replace(struct shell_line_editor* editor, const char* line, size_t size, size_t cursor);

/* End editing at the logical end of the command line. */
bool shell_line_editor_finish(struct shell_line_editor* editor);

/* End editing and display the conventional interactive interrupt marker. */
bool shell_line_editor_interrupt(struct shell_line_editor* editor);
