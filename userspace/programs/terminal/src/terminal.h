#pragma once

#include <base/framebuffer.h>
#include <protocol/terminal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct terminal;

/* Result of copying the current selection into the clipboard. */
enum terminal_clipboard_copy_result {
	TERMINAL_CLIPBOARD_COPY_FAILED,
	TERMINAL_CLIPBOARD_COPY_UNAVAILABLE,
	TERMINAL_CLIPBOARD_COPY_OK,
};

/* Initialize a terminal over one mapped framebuffer and clear its display. */
struct terminal* terminal_create(volatile uint8_t* framebuffer, const struct framebuffer_info_response* info);

/* Read the immutable fixed-width character-cell dimensions. */
bool terminal_get_dimensions(const struct terminal* terminal, size_t* out_columns, size_t* out_rows);

/* Read the current character-cell cursor position and stable line identity. */
bool terminal_get_cursor_position(struct terminal* terminal, size_t* out_column, size_t* out_row, uint64_t* out_line);

/* Release all state owned by a terminal. */
void terminal_destroy(struct terminal* terminal);

/* Consume a prefix of one UTF-8 terminal byte stream write. */
bool terminal_write(struct terminal* terminal, const uint8_t* data, size_t size, size_t* out_written);

/* Replace the transient, underlined UTF-8 composition shown at the cursor. */
bool terminal_set_preedit(struct terminal* terminal, const uint8_t* text, size_t size, size_t selection_end);

/* Move the visible viewport by signed lines, with positive values revealing older output. */
bool terminal_scroll_view(struct terminal* terminal, int64_t lines);

/* Return the visible viewport to the live terminal screen. */
bool terminal_scroll_to_bottom(struct terminal* terminal);

/* Read the active directional selection. */
bool terminal_selection_get(struct terminal* terminal, struct terminal_get_selection_response* out_selection);

/* Replace the active selection with one retained half-open cell range. */
bool terminal_selection_set(struct terminal* terminal, struct terminal_selection_position anchor,
                            struct terminal_selection_position focus);

/* Clear the active selection and restore its cells. */
bool terminal_selection_clear(struct terminal* terminal);

/* Replace the clipboard with the current selection's plain UTF-8 text. */
enum terminal_clipboard_copy_result terminal_clipboard_copy_selection(struct terminal* terminal);

/* Copy the terminal clipboard into a caller-provided buffer. */
bool terminal_clipboard_get(struct terminal* terminal, void* buffer, size_t capacity, size_t* out_size);

/* Allocate a coherent copy of the terminal clipboard for local input handling. */
bool terminal_clipboard_snapshot(struct terminal* terminal, uint8_t** out_text, size_t* out_size);

/* Validate length-delimited UTF-8 clipboard text and its permitted controls. */
bool terminal_clipboard_text_valid(const void* text, size_t size);

/* Atomically replace the terminal clipboard with validated plain UTF-8 text. */
bool terminal_clipboard_set(struct terminal* terminal, const void* text, size_t size);

/* Apply one relative pointer frame and its optional primary-button transition. */
bool terminal_pointer_update(struct terminal* terminal, uint64_t source_id, int64_t delta_x, int64_t delta_y,
                             bool primary_changed, bool primary_pressed);

/* End an unfinished pointer selection after the pointer source is lost. */
void terminal_pointer_cancel(struct terminal* terminal, uint64_t source_id);
