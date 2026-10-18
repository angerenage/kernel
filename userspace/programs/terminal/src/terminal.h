#pragma once

#include <base/framebuffer.h>
#include <protocol/terminal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct terminal;

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

/* Read the active directional selection. */
bool terminal_selection_get(struct terminal* terminal, struct terminal_get_selection_response* out_selection);

/* Replace the active selection with one retained half-open cell range. */
bool terminal_selection_set(struct terminal* terminal, struct terminal_selection_position anchor,
                            struct terminal_selection_position focus);

/* Clear the active selection and restore its cells. */
bool terminal_selection_clear(struct terminal* terminal);
