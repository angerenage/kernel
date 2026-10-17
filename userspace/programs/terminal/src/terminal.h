#pragma once

#include <base/framebuffer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct terminal;

/* Initialize a terminal over one mapped framebuffer and clear its display. */
struct terminal* terminal_create(volatile uint8_t* framebuffer, const struct framebuffer_info_response* info);

/* Read the immutable fixed-width character-cell dimensions. */
bool terminal_get_dimensions(const struct terminal* terminal, size_t* out_columns, size_t* out_rows);

/* Release all state owned by a terminal. */
void terminal_destroy(struct terminal* terminal);

/* Consume a prefix of one UTF-8 terminal byte stream write. */
bool terminal_write(struct terminal* terminal, const uint8_t* data, size_t size, size_t* out_written);

/* Replace the transient, underlined UTF-8 composition shown at the cursor. */
bool terminal_set_preedit(struct terminal* terminal, const uint8_t* text, size_t size, size_t selection_end);
