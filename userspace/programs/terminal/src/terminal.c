#include "terminal.h"

#include <base/math.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyph_cache.h"

#define FRAMEBUFFER_MEMORY_MODEL_RGB 1u
#define TERMINAL_FONT_PATH "/boot/CommitMono.otf"
#define TERMINAL_FONT_PIXEL_HEIGHT 18u
#define TERMINAL_TAB_WIDTH 4u
#define TERMINAL_CSI_PARAMETER_CAPACITY 16u
#define TERMINAL_SCROLLBACK_LINE_CAPACITY 4096u

struct color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

struct terminal_theme {
	struct color background;
	struct color foreground;
	struct color selection_background;
	struct color selection_foreground;
	struct color cursor;
	struct color cursor_text;
	struct color palette[16];
};

struct pixel_format {
	size_t  bytes_per_pixel;
	uint8_t red_size;
	uint8_t red_shift;
	uint8_t green_size;
	uint8_t green_shift;
	uint8_t blue_size;
	uint8_t blue_shift;
};

struct drawing_surface {
	volatile uint8_t*   framebuffer;
	size_t              width;
	size_t              height;
	size_t              pitch;
	struct pixel_format format;
};

struct terminal_cell {
	uint32_t     codepoint;
	struct color foreground;
	struct color background;
	bool         underline;
};

struct terminal_overlay_cell {
	size_t               index;
	struct terminal_cell cell;
};

enum escape_state {
	ESCAPE_STATE_TEXT,
	ESCAPE_STATE_ESCAPE,
	ESCAPE_STATE_CSI,
};

struct terminal {
	struct drawing_surface        surface;
	FT_Library                    library;
	FT_Face                       face;
	struct terminal_glyph_cache*  glyph_cache;
	uint8_t*                      font_data;
	size_t                        cell_width;
	size_t                        cell_height;
	size_t                        ascender;
	size_t                        columns;
	size_t                        rows;
	size_t                        column;
	size_t                        row;
	struct terminal_cell*         cells;
	size_t                        cell_count;
	struct terminal_cell*         history;
	size_t                        history_count;
	size_t                        history_head;
	size_t                        viewport_offset;
	bool                          viewport_dirty;
	size_t                        saved_column;
	size_t                        saved_row;
	bool                          saved_cursor;
	bool                          cursor_visible;
	bool                          cursor_drawn;
	size_t                        cursor_drawn_row;
	size_t                        cursor_drawn_column;
	bool                          wrap_pending;
	bool                          locked;
	uint32_t*                     preedit_codepoints;
	size_t                        preedit_count;
	size_t                        preedit_caret;
	struct terminal_overlay_cell* overlay;
	size_t                        overlay_count;
	bool                          foreground_default;
	bool                          background_default;
	bool                          bold;
	bool                          reverse;
	bool                          underline;
	uint8_t                       foreground;
	uint8_t                       background;
	uint32_t                      utf8_value;
	uint32_t                      utf8_minimum;
	uint8_t                       utf8_remaining;
	enum escape_state             escape_state;
	uint16_t                      csi_parameters[TERMINAL_CSI_PARAMETER_CAPACITY];
	size_t                        csi_parameter_count;
	uint16_t                      csi_value;
	bool                          csi_has_value;
	bool                          csi_invalid;
	bool                          csi_private;
};

static const struct terminal_theme theme = {
	.background           = {0x1e, 0x1e, 0x1e},
	.foreground           = {0xcc, 0xcc, 0xcc},
	.selection_background = {0x3a, 0x3d, 0x41},
	.selection_foreground = {0xe0, 0xe0, 0xe0},
	.cursor               = {0xff, 0xff, 0xff},
	.cursor_text          = {0x00, 0x00, 0x00},
	.palette[0]           = {0x00, 0x00, 0x00}, // Black
	.palette[1]           = {0xd8, 0x2a, 0x2a}, // Red
	.palette[2]           = {0x22, 0xac, 0x3c}, // Green
	.palette[3]           = {0xe6, 0xe6, 0x03}, // Dark yellow
	.palette[4]           = {0x31, 0x45, 0x9c}, // Dark blue
	.palette[5]           = {0xd8, 0x22, 0x7f}, // Magenta
	.palette[6]           = {0x18, 0x99, 0xb9}, // Dark cyan
	.palette[7]           = {0x96, 0x96, 0x96}, // Light gray
	.palette[8]           = {0x50, 0x50, 0x50}, // Dark gray
	.palette[9]           = {0xf7, 0x49, 0x49}, // Light red
	.palette[10]          = {0xac, 0xe9, 0x71}, // Light green
	.palette[11]          = {0xf7, 0xf7, 0x61}, // Yellow
	.palette[12]          = {0x52, 0xa0, 0xf5}, // Blue
	.palette[13]          = {0xd6, 0x70, 0xd6}, // Light magenta
	.palette[14]          = {0x3d, 0xf8, 0xf7}, // Cyan
	.palette[15]          = {0xe5, 0xe5, 0xe5}, // White
};

static bool channel_valid(uint8_t size, uint8_t shift, uint16_t bpp) {
	return size != 0u && size <= 16u && shift < bpp && size <= bpp - shift;
}

static uint64_t channel_mask(uint8_t size, uint8_t shift) {
	return (((uint64_t)1u << size) - 1u) << shift;
}

static bool pixel_format_initialize(const struct framebuffer_info_response* info, struct pixel_format* format) {
	uint64_t red_mask;
	uint64_t green_mask;
	uint64_t blue_mask;

	if (info == NULL || format == NULL || info->memory_model != FRAMEBUFFER_MEMORY_MODEL_RGB || info->bpp == 0u ||
	    info->bpp > 64u || info->bpp % 8u != 0u ||
	    !channel_valid(info->red_mask_size, info->red_mask_shift, info->bpp) ||
	    !channel_valid(info->green_mask_size, info->green_mask_shift, info->bpp) ||
	    !channel_valid(info->blue_mask_size, info->blue_mask_shift, info->bpp))
		return false;
	red_mask   = channel_mask(info->red_mask_size, info->red_mask_shift);
	green_mask = channel_mask(info->green_mask_size, info->green_mask_shift);
	blue_mask  = channel_mask(info->blue_mask_size, info->blue_mask_shift);
	if ((red_mask & green_mask) != 0u || (red_mask & blue_mask) != 0u || (green_mask & blue_mask) != 0u) return false;
	*format = (struct pixel_format){
		.bytes_per_pixel = info->bpp / 8u,
		.red_size        = info->red_mask_size,
		.red_shift       = info->red_mask_shift,
		.green_size      = info->green_mask_size,
		.green_shift     = info->green_mask_shift,
		.blue_size       = info->blue_mask_size,
		.blue_shift      = info->blue_mask_shift,
	};
	return true;
}

static uint64_t encode_channel(uint8_t value, uint8_t size, uint8_t shift) {
	uint64_t maximum = ((uint64_t)1u << size) - 1u;
	return (((uint64_t)value * maximum + 127u) / 255u) << shift;
}

static uint64_t encode_color(const struct pixel_format* format, struct color color) {
	return encode_channel(color.red, format->red_size, format->red_shift) |
	       encode_channel(color.green, format->green_size, format->green_shift) |
	       encode_channel(color.blue, format->blue_size, format->blue_shift);
}

static void write_pixel(volatile uint8_t* destination, size_t size, uint64_t pixel) {
	for (size_t byte = 0u; byte < size; byte++) destination[byte] = (uint8_t)(pixel >> (byte * 8u));
}

static void fill_rectangle(volatile uint8_t* framebuffer, size_t pitch, const struct pixel_format* format, size_t x,
                           size_t y, size_t width, size_t height, struct color color) {
	uint64_t pixel = encode_color(format, color);
	for (size_t row = 0u; row < height; row++) {
		volatile uint8_t* destination = framebuffer + (y + row) * pitch + x * format->bytes_per_pixel;
		for (size_t column = 0u; column < width; column++) {
			write_pixel(destination, format->bytes_per_pixel, pixel);
			destination += format->bytes_per_pixel;
		}
	}
}

static void copy_framebuffer_row(volatile uint8_t* destination, const volatile uint8_t* source, size_t size) {
	if ((((uintptr_t)destination | (uintptr_t)source) & (sizeof(uint64_t) - 1u)) == 0u) {
		size_t words = size / sizeof(uint64_t);
		for (size_t word = 0u; word < words; word++)
			((volatile uint64_t*)destination)[word] = ((const volatile uint64_t*)source)[word];
		destination += words * sizeof(uint64_t);
		source += words * sizeof(uint64_t);
		size -= words * sizeof(uint64_t);
	}
	for (size_t byte = 0u; byte < size; byte++) destination[byte] = source[byte];
}

static struct color blend_color(struct color foreground, struct color background, uint8_t alpha) {
	uint16_t inverse = (uint16_t)(255u - alpha);
	return (struct color){
		.red   = (uint8_t)(((uint16_t)foreground.red * alpha + (uint16_t)background.red * inverse + 127u) / 255u),
		.green = (uint8_t)(((uint16_t)foreground.green * alpha + (uint16_t)background.green * inverse + 127u) / 255u),
		.blue  = (uint8_t)(((uint16_t)foreground.blue * alpha + (uint16_t)background.blue * inverse + 127u) / 255u),
	};
}

static bool draw_glyph(const struct drawing_surface* surface, const struct terminal_glyph* glyph, int64_t x, int64_t y,
                       size_t clip_x, size_t clip_y, size_t clip_width, size_t clip_height, struct color foreground,
                       struct color background) {
	size_t clip_right;
	size_t clip_bottom;

	if (surface == NULL || glyph == NULL || clip_x > surface->width || clip_y > surface->height ||
	    clip_width > surface->width - clip_x || clip_height > surface->height - clip_y)
		return false;
	clip_right  = clip_x + clip_width;
	clip_bottom = clip_y + clip_height;
	if (glyph->width == 0u || glyph->height == 0u) return true;
	if (glyph->alpha == NULL) return false;

	for (size_t row = 0u; row < glyph->height; row++) {
		for (size_t column = 0u; column < glyph->width; column++) {
			int64_t destination_x = x + (int64_t)column;
			int64_t destination_y = y + (int64_t)row;
			uint8_t alpha         = glyph->alpha[row * glyph->width + column];

			if (alpha == 0u || destination_x < 0 || destination_y < 0 || (uint64_t)destination_x < clip_x ||
			    (uint64_t)destination_x >= clip_right || (uint64_t)destination_y < clip_y ||
			    (uint64_t)destination_y >= clip_bottom)
				continue;
			volatile uint8_t* destination = surface->framebuffer + (size_t)destination_y * surface->pitch +
			                                (size_t)destination_x * surface->format.bytes_per_pixel;
			write_pixel(destination,
			            surface->format.bytes_per_pixel,
			            encode_color(&surface->format, blend_color(foreground, background, alpha)));
		}
	}
	return true;
}

static bool load_font(uint8_t** data, size_t* size) {
	FILE*    file = NULL;
	long     file_size;
	uint8_t* buffer  = NULL;
	bool     success = false;

	if (data == NULL || size == NULL) return false;
	*data = NULL;
	*size = 0u;
	file  = fopen(TERMINAL_FONT_PATH, "rb");
	if (file == NULL || fseek(file, 0l, SEEK_END) != 0) goto cleanup;
	file_size = ftell(file);
	if (file_size <= 0l || fseek(file, 0l, SEEK_SET) != 0) goto cleanup;
	buffer = malloc((size_t)file_size);
	if (buffer == NULL || fread(buffer, 1u, (size_t)file_size, file) != (size_t)file_size) goto cleanup;
	*data   = buffer;
	*size   = (size_t)file_size;
	buffer  = NULL;
	success = true;

cleanup:
	free(buffer);
	if (file != NULL && fclose(file) != 0) success = false;
	if (!success) {
		free(*data);
		*data = NULL;
		*size = 0u;
	}
	return success;
}

static void reset_attributes(struct terminal* terminal) {
	terminal->foreground_default = true;
	terminal->background_default = true;
	terminal->foreground         = 7u;
	terminal->background         = 0u;
	terminal->bold               = false;
	terminal->reverse            = false;
	terminal->underline          = false;
}

static struct color selected_foreground(const struct terminal* terminal) {
	struct color foreground =
		terminal->foreground_default
	        ? theme.foreground
	        : theme.palette[terminal->foreground < 8u && terminal->bold ? terminal->foreground + 8u
	                                                                    : terminal->foreground];
	struct color background = terminal->background_default ? theme.background : theme.palette[terminal->background];
	return terminal->reverse ? background : foreground;
}

static struct color selected_background(const struct terminal* terminal) {
	struct color foreground =
		terminal->foreground_default
	        ? theme.foreground
	        : theme.palette[terminal->foreground < 8u && terminal->bold ? terminal->foreground + 8u
	                                                                    : terminal->foreground];
	struct color background = terminal->background_default ? theme.background : theme.palette[terminal->background];
	return terminal->reverse ? foreground : background;
}

static struct terminal_cell blank_cell(const struct terminal* terminal) {
	return (struct terminal_cell){
		.codepoint  = ' ',
		.foreground = selected_foreground(terminal),
		.background = selected_background(terminal),
	};
}

static bool draw_cell_value(struct terminal* terminal, const struct terminal_cell* cell, size_t row, size_t column,
                            bool cursor) {
	struct color foreground;
	struct color background;
	size_t       cell_x = column * terminal->cell_width;
	size_t       cell_y = row * terminal->cell_height;

	foreground = cursor ? theme.cursor_text : cell->foreground;
	background = cursor ? theme.cursor : cell->background;
	fill_rectangle(terminal->surface.framebuffer,
	               terminal->surface.pitch,
	               &terminal->surface.format,
	               cell_x,
	               cell_y,
	               terminal->cell_width,
	               terminal->cell_height,
	               background);
	if (cell->codepoint != ' ') {
		const struct terminal_glyph* glyph;
		int64_t                      glyph_x;
		int64_t                      glyph_y;

		if (!terminal_glyph_cache_get(terminal->glyph_cache, cell->codepoint, &glyph)) return false;
		glyph_x = (int64_t)cell_x + glyph->left;
		glyph_y = (int64_t)cell_y + (int64_t)terminal->ascender - glyph->top;
		if (!draw_glyph(&terminal->surface,
		                glyph,
		                glyph_x,
		                glyph_y,
		                cell_x,
		                cell_y,
		                terminal->cell_width,
		                terminal->cell_height,
		                foreground,
		                background))
			return false;
	}
	if (cell->underline && !cursor) {
		size_t underline_y = cell_y + terminal->ascender + 1u;
		if (underline_y >= cell_y + terminal->cell_height) underline_y = cell_y + terminal->cell_height - 1u;
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               cell_x,
		               underline_y,
		               terminal->cell_width,
		               1u,
		               foreground);
	}
	return true;
}

static bool live_row_visible(const struct terminal* terminal, size_t row) {
	return terminal->viewport_offset < terminal->rows && row < terminal->rows - terminal->viewport_offset;
}

static bool draw_cell(struct terminal* terminal, size_t row, size_t column, bool cursor) {
	if (terminal->viewport_offset != 0u) {
		if (live_row_visible(terminal, row)) terminal->viewport_dirty = true;
		return true;
	}
	return draw_cell_value(terminal, &terminal->cells[row * terminal->columns + column], row, column, cursor);
}

static const struct terminal_cell* history_line(const struct terminal* terminal, size_t line) {
	size_t slot = (terminal->history_head + line) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
	return terminal->history + slot * terminal->columns;
}

static bool redraw_viewport_row(struct terminal* terminal, size_t row) {
	size_t                      line = terminal->history_count - terminal->viewport_offset + row;
	const struct terminal_cell* cells;

	if (line < terminal->history_count) cells = history_line(terminal, line);
	else cells = terminal->cells + (line - terminal->history_count) * terminal->columns;
	for (size_t column = 0u; column < terminal->columns; column++)
		if (!draw_cell_value(terminal, &cells[column], row, column, false)) return false;
	return true;
}

static bool redraw_viewport(struct terminal* terminal) {
	for (size_t row = 0u; row < terminal->rows; row++)
		if (!redraw_viewport_row(terminal, row)) return false;
	terminal->viewport_dirty = false;
	return true;
}

static bool move_viewport(struct terminal* terminal, bool older, size_t lines) {
	size_t width  = terminal->columns * terminal->cell_width * terminal->surface.format.bytes_per_pixel;
	size_t height = terminal->rows * terminal->cell_height;
	size_t pixel_lines;

	if (lines >= terminal->rows || mul_overflow_size(lines, terminal->cell_height, &pixel_lines) ||
	    pixel_lines >= height)
		return redraw_viewport(terminal);
	if (older) {
		for (size_t row = height - pixel_lines; row != 0u; row--) {
			size_t            source_row = row - 1u;
			volatile uint8_t* destination =
				terminal->surface.framebuffer + (source_row + pixel_lines) * terminal->surface.pitch;
			volatile uint8_t* source = terminal->surface.framebuffer + source_row * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
		for (size_t row = 0u; row < lines; row++)
			if (!redraw_viewport_row(terminal, row)) return false;
	}
	else {
		for (size_t row = 0u; row < height - pixel_lines; row++) {
			volatile uint8_t* destination = terminal->surface.framebuffer + row * terminal->surface.pitch;
			volatile uint8_t* source = terminal->surface.framebuffer + (row + pixel_lines) * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
		for (size_t row = terminal->rows - lines; row < terminal->rows; row++)
			if (!redraw_viewport_row(terminal, row)) return false;
	}
	terminal->viewport_dirty = false;
	return true;
}

static const struct terminal_cell* overlay_cell_at(const struct terminal* terminal, size_t index) {
	for (size_t i = 0u; i < terminal->overlay_count; i++)
		if (terminal->overlay[i].index == index) return &terminal->overlay[i].cell;
	return NULL;
}

static bool redraw_screen_cell(struct terminal* terminal, size_t row, size_t column, bool cursor) {
	size_t                      index = row * terminal->columns + column;
	const struct terminal_cell* cell  = overlay_cell_at(terminal, index);
	return cell == NULL ? draw_cell(terminal, row, column, cursor)
	                    : draw_cell_value(terminal, cell, row, column, cursor);
}

static bool hide_cursor(struct terminal* terminal) {
	if (!terminal->cursor_drawn) return true;
	terminal->cursor_drawn = false;
	return redraw_screen_cell(terminal, terminal->cursor_drawn_row, terminal->cursor_drawn_column, false);
}

static bool show_cursor(struct terminal* terminal) {
	size_t row    = terminal->row;
	size_t column = terminal->column;
	if (!terminal->cursor_visible || terminal->cursor_drawn || terminal->viewport_offset != 0u) return true;
	if (terminal->overlay_count != 0u) {
		size_t caret =
			terminal->preedit_caret < terminal->overlay_count ? terminal->preedit_caret : terminal->overlay_count;
		size_t index = terminal->overlay[0].index + caret;
		if (index >= terminal->cell_count) index = terminal->cell_count - 1u;
		row    = index / terminal->columns;
		column = index % terminal->columns;
	}
	if (!redraw_screen_cell(terminal, row, column, true)) return false;
	terminal->cursor_drawn        = true;
	terminal->cursor_drawn_row    = row;
	terminal->cursor_drawn_column = column;
	return true;
}

static bool erase_overlay(struct terminal* terminal) {
	bool success = true;
	for (size_t i = 0u; i < terminal->overlay_count; i++) {
		size_t index = terminal->overlay[i].index;
		if (terminal->viewport_offset == 0u &&
		    !draw_cell(terminal, index / terminal->columns, index % terminal->columns, false))
			success = false;
	}
	terminal->overlay_count = 0u;
	return success;
}

static bool draw_overlay(struct terminal* terminal) {
	if (terminal->preedit_count == 0u) return true;
	size_t first = terminal->row * terminal->columns + terminal->column;
	if (terminal->wrap_pending) {
		if (first + 1u >= terminal->cell_count) return true;
		first++;
	}
	terminal->overlay_count = terminal->preedit_count;
	if (terminal->overlay_count > terminal->cell_count - first) terminal->overlay_count = terminal->cell_count - first;
	for (size_t i = 0u; i < terminal->overlay_count; i++) {
		terminal->overlay[i] = (struct terminal_overlay_cell){
			.index = first + i,
			.cell =
				{
					   .codepoint  = terminal->preedit_codepoints[i],
					   .foreground = selected_foreground(terminal),
					   .background = selected_background(terminal),
					   .underline  = true,
					   },
		};
		if (terminal->viewport_offset == 0u && !draw_cell_value(terminal,
		                                                        &terminal->overlay[i].cell,
		                                                        terminal->overlay[i].index / terminal->columns,
		                                                        terminal->overlay[i].index % terminal->columns,
		                                                        false))
			return false;
	}
	return true;
}

static void terminal_lock(struct terminal* terminal) {
	while (__atomic_test_and_set(&terminal->locked, __ATOMIC_ACQUIRE)) __asm__ volatile("" ::: "memory");
}

static void terminal_unlock(struct terminal* terminal) {
	__atomic_clear(&terminal->locked, __ATOMIC_RELEASE);
}

static void clear_display(struct terminal* terminal) {
	struct terminal_cell blank = blank_cell(terminal);

	if (terminal->viewport_offset == 0u)
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               0u,
		               0u,
		               terminal->surface.width,
		               terminal->surface.height,
		               blank.background);
	else if (terminal->viewport_offset < terminal->rows) terminal->viewport_dirty = true;
	for (size_t index = 0u; index < terminal->cell_count; index++) terminal->cells[index] = blank;
	terminal->column       = 0u;
	terminal->row          = 0u;
	terminal->wrap_pending = false;
}

static void append_history_line(struct terminal* terminal, const struct terminal_cell* cells) {
	bool   full = terminal->history_count == TERMINAL_SCROLLBACK_LINE_CAPACITY;
	size_t slot;

	if (full) {
		slot                   = terminal->history_head;
		terminal->history_head = (terminal->history_head + 1u) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
	}
	else {
		slot = (terminal->history_head + terminal->history_count) % TERMINAL_SCROLLBACK_LINE_CAPACITY;
		terminal->history_count++;
	}
	memcpy(terminal->history + slot * terminal->columns, cells, terminal->columns * sizeof(*cells));
	if (terminal->viewport_offset == 0u) return;
	if (!full || terminal->viewport_offset < terminal->history_count) terminal->viewport_offset++;
	else terminal->viewport_dirty = true;
}

static void scroll_screen(struct terminal* terminal) {
	size_t               width  = terminal->columns * terminal->cell_width * terminal->surface.format.bytes_per_pixel;
	size_t               height = terminal->rows * terminal->cell_height;
	size_t               copied_rows = height - terminal->cell_height;
	struct terminal_cell blank       = blank_cell(terminal);

	append_history_line(terminal, terminal->cells);
	if (terminal->viewport_offset == 0u) {
		for (size_t row = 0u; row < copied_rows; row++) {
			volatile uint8_t* destination = terminal->surface.framebuffer + row * terminal->surface.pitch;
			volatile uint8_t* source =
				terminal->surface.framebuffer + (row + terminal->cell_height) * terminal->surface.pitch;
			copy_framebuffer_row(destination, source, width);
		}
	}
	memmove(terminal->cells,
	        terminal->cells + terminal->columns,
	        (terminal->cell_count - terminal->columns) * sizeof(*terminal->cells));
	for (size_t index = terminal->cell_count - terminal->columns; index < terminal->cell_count; index++)
		terminal->cells[index] = blank;
	if (terminal->viewport_offset == 0u)
		fill_rectangle(terminal->surface.framebuffer,
		               terminal->surface.pitch,
		               &terminal->surface.format,
		               0u,
		               copied_rows,
		               terminal->columns * terminal->cell_width,
		               terminal->cell_height,
		               blank.background);
}

static void next_line(struct terminal* terminal) {
	terminal->column       = 0u;
	terminal->wrap_pending = false;
	if (terminal->row + 1u < terminal->rows) terminal->row++;
	else scroll_screen(terminal);
}

static bool draw_codepoint(struct terminal* terminal, uint32_t codepoint) {
	struct terminal_cell* cell;

	if (terminal->wrap_pending) next_line(terminal);
	cell  = &terminal->cells[terminal->row * terminal->columns + terminal->column];
	*cell = (struct terminal_cell){
		.codepoint  = codepoint,
		.foreground = selected_foreground(terminal),
		.background = selected_background(terminal),
		.underline  = terminal->underline,
	};
	if (!draw_cell(terminal, terminal->row, terminal->column, false)) return false;
	if (terminal->column + 1u == terminal->columns) terminal->wrap_pending = true;
	else terminal->column++;
	return true;
}

static bool emit_replacement(struct terminal* terminal) {
	terminal->utf8_value     = 0u;
	terminal->utf8_minimum   = 0u;
	terminal->utf8_remaining = 0u;
	return draw_codepoint(terminal, 0xfffdu);
}

static void csi_reset(struct terminal* terminal) {
	terminal->csi_parameter_count = 0u;
	terminal->csi_value           = 0u;
	terminal->csi_has_value       = false;
	terminal->csi_invalid         = false;
	terminal->csi_private         = false;
}

static void csi_push(struct terminal* terminal) {
	if (terminal->csi_parameter_count >= TERMINAL_CSI_PARAMETER_CAPACITY) {
		terminal->csi_invalid = true;
		return;
	}
	terminal->csi_parameters[terminal->csi_parameter_count++] = terminal->csi_has_value ? terminal->csi_value : 0u;
	terminal->csi_value                                       = 0u;
	terminal->csi_has_value                                   = false;
}

static void apply_sgr(struct terminal* terminal) {
	if (terminal->csi_parameter_count == 0u) csi_push(terminal);
	if (terminal->csi_invalid || terminal->csi_private) return;
	for (size_t index = 0u; index < terminal->csi_parameter_count; index++) {
		uint16_t parameter = terminal->csi_parameters[index];
		if (parameter == 0u) reset_attributes(terminal);
		else if (parameter == 1u) terminal->bold = true;
		else if (parameter == 4u) terminal->underline = true;
		else if (parameter == 7u) terminal->reverse = true;
		else if (parameter == 22u) terminal->bold = false;
		else if (parameter == 24u) terminal->underline = false;
		else if (parameter == 27u) terminal->reverse = false;
		else if (parameter >= 30u && parameter <= 37u) {
			terminal->foreground_default = false;
			terminal->foreground         = (uint8_t)(parameter - 30u);
		}
		else if (parameter == 39u) terminal->foreground_default = true;
		else if (parameter >= 40u && parameter <= 47u) {
			terminal->background_default = false;
			terminal->background         = (uint8_t)(parameter - 40u);
		}
		else if (parameter == 49u) terminal->background_default = true;
		else if (parameter >= 90u && parameter <= 97u) {
			terminal->foreground_default = false;
			terminal->foreground         = (uint8_t)(parameter - 90u + 8u);
		}
		else if (parameter >= 100u && parameter <= 107u) {
			terminal->background_default = false;
			terminal->background         = (uint8_t)(parameter - 100u + 8u);
		}
		else if ((parameter == 38u || parameter == 48u) && index + 2u < terminal->csi_parameter_count &&
		         terminal->csi_parameters[index + 1u] == 5u && terminal->csi_parameters[index + 2u] < 16u) {
			if (parameter == 38u) {
				terminal->foreground_default = false;
				terminal->foreground         = (uint8_t)terminal->csi_parameters[index + 2u];
			}
			else {
				terminal->background_default = false;
				terminal->background         = (uint8_t)terminal->csi_parameters[index + 2u];
			}
			index += 2u;
		}
	}
}

static size_t csi_parameter(const struct terminal* terminal, size_t index, size_t default_value) {
	if (index >= terminal->csi_parameter_count || terminal->csi_parameters[index] == 0u) return default_value;
	return terminal->csi_parameters[index];
}

static bool erase_cells(struct terminal* terminal, size_t first, size_t last) {
	struct terminal_cell blank = blank_cell(terminal);

	if (first >= terminal->cell_count || last >= terminal->cell_count || first > last) return false;
	for (size_t index = first; index <= last; index++) {
		terminal->cells[index] = blank;
		if (!draw_cell(terminal, index / terminal->columns, index % terminal->columns, false)) return false;
	}
	return true;
}

static bool erase_display(struct terminal* terminal, size_t mode) {
	size_t cursor = terminal->row * terminal->columns + terminal->column;

	if (mode == 0u) return erase_cells(terminal, cursor, terminal->cell_count - 1u);
	if (mode == 1u) return erase_cells(terminal, 0u, cursor);
	if (mode == 2u || mode == 3u) return erase_cells(terminal, 0u, terminal->cell_count - 1u);
	return true;
}

static bool erase_line(struct terminal* terminal, size_t mode) {
	size_t first = terminal->row * terminal->columns;
	size_t last  = first + terminal->columns - 1u;

	if (mode == 0u) first += terminal->column;
	else if (mode == 1u) last = first + terminal->column;
	else if (mode != 2u) return true;
	return erase_cells(terminal, first, last);
}

static void move_cursor(struct terminal* terminal, size_t row, size_t column) {
	terminal->row          = row < terminal->rows ? row : terminal->rows - 1u;
	terminal->column       = column < terminal->columns ? column : terminal->columns - 1u;
	terminal->wrap_pending = false;
}

static bool apply_csi(struct terminal* terminal, uint8_t final) {
	size_t amount;

	if (terminal->csi_invalid) return true;
	if (terminal->csi_private) {
		if ((final == 'h' || final == 'l') && terminal->csi_parameter_count == 1u && terminal->csi_parameters[0] == 25u)
			terminal->cursor_visible = final == 'h';
		return true;
	}
	if (final == 'm') {
		apply_sgr(terminal);
		return true;
	}
	amount = csi_parameter(terminal, 0u, 1u);
	if (final == 'A') {
		terminal->row          = amount > terminal->row ? 0u : terminal->row - amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'B') {
		terminal->row = amount >= terminal->rows - terminal->row ? terminal->rows - 1u : terminal->row + amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'C') {
		terminal->column =
			amount >= terminal->columns - terminal->column ? terminal->columns - 1u : terminal->column + amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'D') {
		terminal->column       = amount > terminal->column ? 0u : terminal->column - amount;
		terminal->wrap_pending = false;
	}
	else if (final == 'E')
		move_cursor(
			terminal, amount >= terminal->rows - terminal->row ? terminal->rows - 1u : terminal->row + amount, 0u);
	else if (final == 'F') move_cursor(terminal, amount > terminal->row ? 0u : terminal->row - amount, 0u);
	else if (final == 'G') move_cursor(terminal, terminal->row, amount - 1u);
	else if (final == 'd') move_cursor(terminal, amount - 1u, terminal->column);
	else if (final == 'H' || final == 'f')
		move_cursor(terminal, csi_parameter(terminal, 0u, 1u) - 1u, csi_parameter(terminal, 1u, 1u) - 1u);
	else if (final == 'J') return erase_display(terminal, csi_parameter(terminal, 0u, 0u));
	else if (final == 'K') return erase_line(terminal, csi_parameter(terminal, 0u, 0u));
	else if (final == 's') {
		terminal->saved_row    = terminal->row;
		terminal->saved_column = terminal->column;
		terminal->saved_cursor = true;
	}
	else if (final == 'u' && terminal->saved_cursor) move_cursor(terminal, terminal->saved_row, terminal->saved_column);
	return true;
}

static bool process_text_byte(struct terminal* terminal, uint8_t byte);

static bool process_escape_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->escape_state == ESCAPE_STATE_ESCAPE) {
		terminal->escape_state = byte == '[' ? ESCAPE_STATE_CSI : ESCAPE_STATE_TEXT;
		if (byte == '[') csi_reset(terminal);
		else if (byte == '7') {
			terminal->saved_row    = terminal->row;
			terminal->saved_column = terminal->column;
			terminal->saved_cursor = true;
		}
		else if (byte == '8' && terminal->saved_cursor)
			move_cursor(terminal, terminal->saved_row, terminal->saved_column);
		return true;
	}
	if (byte == '?' && terminal->csi_parameter_count == 0u && !terminal->csi_has_value) {
		terminal->csi_private = true;
		return true;
	}
	if (byte >= '0' && byte <= '9') {
		uint16_t digit = (uint16_t)(byte - '0');
		if (terminal->csi_value > (UINT16_MAX - digit) / 10u) terminal->csi_invalid = true;
		else terminal->csi_value = (uint16_t)(terminal->csi_value * 10u + digit);
		terminal->csi_has_value = true;
		return true;
	}
	if (byte == ';') {
		csi_push(terminal);
		return true;
	}
	if (byte >= 0x40u && byte <= 0x7eu) {
		if (terminal->csi_has_value || terminal->csi_parameter_count != 0u) csi_push(terminal);
		bool success           = apply_csi(terminal, byte);
		terminal->escape_state = ESCAPE_STATE_TEXT;
		return success;
	}
	if (byte < 0x20u || byte > 0x7eu) terminal->escape_state = ESCAPE_STATE_TEXT;
	return true;
}

static bool process_utf8_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->utf8_remaining != 0u) {
		if ((byte & 0xc0u) != 0x80u) {
			if (!emit_replacement(terminal)) return false;
			return process_text_byte(terminal, byte);
		}
		terminal->utf8_value = (terminal->utf8_value << 6u) | (byte & 0x3fu);
		terminal->utf8_remaining--;
		if (terminal->utf8_remaining != 0u) return true;
		uint32_t codepoint     = terminal->utf8_value;
		uint32_t minimum       = terminal->utf8_minimum;
		terminal->utf8_value   = 0u;
		terminal->utf8_minimum = 0u;
		if (codepoint < minimum || codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu))
			return draw_codepoint(terminal, 0xfffdu);
		return draw_codepoint(terminal, codepoint);
	}
	if (byte < 0x80u) return draw_codepoint(terminal, byte);
	if (byte >= 0xc2u && byte <= 0xdfu) {
		terminal->utf8_value     = byte & 0x1fu;
		terminal->utf8_minimum   = 0x80u;
		terminal->utf8_remaining = 1u;
		return true;
	}
	if (byte >= 0xe0u && byte <= 0xefu) {
		terminal->utf8_value     = byte & 0x0fu;
		terminal->utf8_minimum   = 0x800u;
		terminal->utf8_remaining = 2u;
		return true;
	}
	if (byte >= 0xf0u && byte <= 0xf4u) {
		terminal->utf8_value     = byte & 0x07u;
		terminal->utf8_minimum   = 0x10000u;
		terminal->utf8_remaining = 3u;
		return true;
	}
	return draw_codepoint(terminal, 0xfffdu);
}

static bool process_text_byte(struct terminal* terminal, uint8_t byte) {
	if (terminal->escape_state != ESCAPE_STATE_TEXT) return process_escape_byte(terminal, byte);
	if (byte == 0x1bu) {
		if (terminal->utf8_remaining != 0u && !emit_replacement(terminal)) return false;
		terminal->escape_state = ESCAPE_STATE_ESCAPE;
		return true;
	}
	if (byte < 0x20u || byte == 0x7fu) {
		if (terminal->utf8_remaining != 0u && !emit_replacement(terminal)) return false;
		if (byte == '\n') next_line(terminal);
		else if (byte == '\r') {
			terminal->column       = 0u;
			terminal->wrap_pending = false;
		}
		else if (byte == '\b') {
			if (terminal->wrap_pending) terminal->wrap_pending = false;
			else if (terminal->column != 0u) terminal->column--;
		}
		else if (byte == '\t') {
			size_t count = TERMINAL_TAB_WIDTH - terminal->column % TERMINAL_TAB_WIDTH;
			for (size_t index = 0u; index < count; index++) {
				if (!draw_codepoint(terminal, ' ')) return false;
			}
		}
		else if (byte == '\f') clear_display(terminal);
		return true;
	}
	return process_utf8_byte(terminal, byte);
}

static bool positive_metric_pixels(FT_Pos value, size_t* out_pixels) {
	uintmax_t raw;
	uintmax_t pixels;

	if (value <= 0 || out_pixels == NULL) return false;
	raw    = (uintmax_t)value;
	pixels = raw / 64u + (raw % 64u != 0u ? 1u : 0u);
	if (pixels > SIZE_MAX) return false;
	*out_pixels = (size_t)pixels;
	return true;
}

static bool descender_metric_pixels(FT_Pos value, size_t* out_pixels) {
	uintmax_t raw;
	uintmax_t pixels;

	if (value > 0 || out_pixels == NULL) return false;
	raw    = value == LONG_MIN ? (uintmax_t)LONG_MAX + 1u : (uintmax_t)-value;
	pixels = raw / 64u + (raw % 64u != 0u ? 1u : 0u);
	if (pixels > SIZE_MAX) return false;
	*out_pixels = (size_t)pixels;
	return true;
}

struct terminal* terminal_create(volatile uint8_t* framebuffer, const struct framebuffer_info_response* info) {
	struct terminal* terminal = NULL;
	size_t           font_size;
	size_t           metrics_height;
	size_t           descender;
	size_t           glyph_height;
	size_t           visible_row_size;
	size_t           visible_size;
	size_t           history_cell_count;
	FT_Pos           advance;

	if (framebuffer == NULL || info == NULL) return NULL;
	terminal = calloc(1u, sizeof(*terminal));
	if (terminal == NULL || !pixel_format_initialize(info, &terminal->surface.format) ||
	    (uint64_t)(size_t)info->width != info->width || (uint64_t)(size_t)info->height != info->height ||
	    (uint64_t)(size_t)info->pitch != info->pitch || (uint64_t)(size_t)info->size != info->size)
		goto fail;
	terminal->surface = (struct drawing_surface){
		.framebuffer = framebuffer,
		.width       = (size_t)info->width,
		.height      = (size_t)info->height,
		.pitch       = (size_t)info->pitch,
		.format      = terminal->surface.format,
	};
	if (terminal->surface.width == 0u || terminal->surface.height == 0u ||
	    mul_overflow_size(terminal->surface.width, terminal->surface.format.bytes_per_pixel, &visible_row_size) ||
	    visible_row_size > terminal->surface.pitch ||
	    mul_overflow_size(terminal->surface.height, terminal->surface.pitch, &visible_size) ||
	    visible_size > (size_t)info->size || FT_Init_FreeType(&terminal->library) != FT_Err_Ok ||
	    !load_font(&terminal->font_data, &font_size) || font_size > LONG_MAX ||
	    FT_New_Memory_Face(terminal->library, terminal->font_data, (FT_Long)font_size, 0, &terminal->face) !=
	        FT_Err_Ok ||
	    FT_Set_Pixel_Sizes(terminal->face, 0u, TERMINAL_FONT_PIXEL_HEIGHT) != FT_Err_Ok ||
	    terminal->face->size->metrics.height <= 0 || terminal->face->size->metrics.ascender <= 0 ||
	    terminal->face->size->metrics.descender > 0 || FT_Load_Char(terminal->face, 'M', FT_LOAD_DEFAULT) != FT_Err_Ok)
		goto fail;
	advance = terminal->face->glyph->advance.x;
	if (advance <= 0) goto fail;
	if (!positive_metric_pixels(advance, &terminal->cell_width) ||
	    !positive_metric_pixels(terminal->face->size->metrics.height, &metrics_height) ||
	    !positive_metric_pixels(terminal->face->size->metrics.ascender, &terminal->ascender) ||
	    !descender_metric_pixels(terminal->face->size->metrics.descender, &descender) ||
	    add_overflow_size(terminal->ascender, descender, &glyph_height))
		goto fail;
	terminal->cell_height = metrics_height > glyph_height ? metrics_height : glyph_height;
	if (terminal->cell_width == 0u || terminal->cell_height == 0u || terminal->ascender > terminal->cell_height)
		goto fail;
	terminal->columns = terminal->surface.width / terminal->cell_width;
	terminal->rows    = terminal->surface.height / terminal->cell_height;
	if (terminal->columns == 0u || terminal->rows == 0u ||
	    mul_overflow_size(terminal->columns, terminal->rows, &terminal->cell_count) ||
	    terminal->cell_count > SIZE_MAX / sizeof(*terminal->cells) ||
	    mul_overflow_size(terminal->columns, TERMINAL_SCROLLBACK_LINE_CAPACITY, &history_cell_count) ||
	    history_cell_count > SIZE_MAX / sizeof(*terminal->history))
		goto fail;
	terminal->cells       = calloc(terminal->cell_count, sizeof(*terminal->cells));
	terminal->history     = malloc(history_cell_count * sizeof(*terminal->history));
	terminal->glyph_cache = terminal_glyph_cache_create(terminal->face);
	if (terminal->cells == NULL || terminal->history == NULL || terminal->glyph_cache == NULL) goto fail;
	reset_attributes(terminal);
	terminal->cursor_visible = true;
	clear_display(terminal);
	if (!show_cursor(terminal)) goto fail;
	return terminal;

fail:
	terminal_destroy(terminal);
	return NULL;
}

void terminal_destroy(struct terminal* terminal) {
	if (terminal == NULL) return;
	terminal_glyph_cache_destroy(terminal->glyph_cache);
	if (terminal->face != NULL) (void)FT_Done_Face(terminal->face);
	if (terminal->library != NULL) (void)FT_Done_FreeType(terminal->library);
	free(terminal->font_data);
	free(terminal->cells);
	free(terminal->history);
	free(terminal->preedit_codepoints);
	free(terminal->overlay);
	free(terminal);
}

bool terminal_get_dimensions(const struct terminal* terminal, size_t* out_columns, size_t* out_rows) {
	if (terminal == NULL || out_columns == NULL || out_rows == NULL || terminal->columns == 0u || terminal->rows == 0u)
		return false;
	*out_columns = terminal->columns;
	*out_rows    = terminal->rows;
	return true;
}

bool terminal_get_cursor_position(struct terminal* terminal, size_t* out_column, size_t* out_row) {
	if (terminal == NULL || out_column == NULL || out_row == NULL) return false;
	terminal_lock(terminal);
	*out_column = terminal->column;
	*out_row    = terminal->row;
	terminal_unlock(terminal);
	return true;
}

bool terminal_write(struct terminal* terminal, const uint8_t* data, size_t size, size_t* out_written) {
	size_t written = 0u;
	bool   success;

	if (out_written == NULL) return false;
	*out_written = 0u;
	if (terminal == NULL || (size != 0u && data == NULL)) return false;
	if (size == 0u) return true;
	terminal_lock(terminal);
	if (!hide_cursor(terminal) || !erase_overlay(terminal)) {
		terminal_unlock(terminal);
		return false;
	}
	while (written < size && process_text_byte(terminal, data[written])) written++;
	*out_written = written;
	success      = (!terminal->viewport_dirty || redraw_viewport(terminal)) && draw_overlay(terminal) &&
	               show_cursor(terminal) && written == size;
	terminal_unlock(terminal);
	return success;
}

static bool decode_utf8(const uint8_t* text, size_t size, uint32_t* codepoints, size_t* out_count, size_t selection_end,
                        size_t* out_caret) {
	size_t offset = 0u;
	size_t count  = 0u;
	size_t caret  = SIZE_MAX;

	if (out_count == NULL || out_caret == NULL || selection_end > size ||
	    (size != 0u && (text == NULL || codepoints == NULL)))
		return false;
	while (offset < size) {
		uint8_t  first = text[offset];
		uint32_t value;
		size_t   length;
		if (offset == selection_end) caret = count;
		if (first < 0x80u) {
			value  = first;
			length = 1u;
		}
		else if (first >= 0xc2u && first <= 0xdfu) {
			value  = first & 0x1fu;
			length = 2u;
		}
		else if (first >= 0xe0u && first <= 0xefu) {
			value  = first & 0x0fu;
			length = 3u;
		}
		else if (first >= 0xf0u && first <= 0xf4u) {
			value  = first & 0x07u;
			length = 4u;
		}
		else return false;
		if (length > size - offset || (selection_end > offset && selection_end < offset + length)) return false;
		for (size_t i = 1u; i < length; i++) {
			if ((text[offset + i] & 0xc0u) != 0x80u) return false;
			value = (value << 6u) | (text[offset + i] & 0x3fu);
		}
		uint32_t minimum = length == 1u ? 0u : length == 2u ? 0x80u : length == 3u ? 0x800u : 0x10000u;
		if (value < minimum || value > 0x10ffffu || (value >= 0xd800u && value <= 0xdfffu)) return false;
		codepoints[count++] = value;
		offset += length;
	}
	if (selection_end == size) caret = count;
	if (caret == SIZE_MAX) return false;
	*out_count = count;
	*out_caret = caret;
	return true;
}

bool terminal_set_preedit(struct terminal* terminal, const uint8_t* text, size_t size, size_t selection_end) {
	uint32_t*                     codepoints = NULL;
	struct terminal_overlay_cell* overlay    = NULL;
	size_t                        count;
	size_t                        caret;
	bool                          success;

	if (terminal == NULL || selection_end > size || (size != 0u && text == NULL) ||
	    size > SIZE_MAX / sizeof(*codepoints) || size > SIZE_MAX / sizeof(*overlay))
		return false;
	if (size != 0u) {
		codepoints = malloc(size * sizeof(*codepoints));
		overlay    = malloc(size * sizeof(*overlay));
		if (codepoints == NULL || overlay == NULL ||
		    !decode_utf8(text, size, codepoints, &count, selection_end, &caret)) {
			free(codepoints);
			free(overlay);
			return false;
		}
	}
	else {
		count = 0u;
		caret = 0u;
	}

	terminal_lock(terminal);
	success = hide_cursor(terminal) && erase_overlay(terminal);
	if (success) {
		free(terminal->preedit_codepoints);
		free(terminal->overlay);
		terminal->preedit_codepoints = codepoints;
		terminal->preedit_count      = count;
		terminal->preedit_caret      = caret;
		terminal->overlay            = overlay;
		codepoints                   = NULL;
		overlay                      = NULL;
		success                      = draw_overlay(terminal) && show_cursor(terminal);
	}
	terminal_unlock(terminal);
	free(codepoints);
	free(overlay);
	return success;
}

bool terminal_scroll_view(struct terminal* terminal, int64_t lines) {
	size_t amount;
	size_t offset;
	size_t moved;
	bool   success;

	if (terminal == NULL) return false;
	if (lines > 0) amount = (uint64_t)lines > SIZE_MAX ? SIZE_MAX : (size_t)lines;
	else {
		uint64_t magnitude = lines == INT64_MIN ? (uint64_t)INT64_MAX + 1u : (uint64_t)-lines;
		amount             = magnitude > SIZE_MAX ? SIZE_MAX : (size_t)magnitude;
	}
	terminal_lock(terminal);
	if (!hide_cursor(terminal) || !erase_overlay(terminal)) {
		terminal_unlock(terminal);
		return false;
	}
	offset = terminal->viewport_offset;
	if (lines > 0)
		terminal->viewport_offset =
			amount > terminal->history_count - offset ? terminal->history_count : offset + amount;
	else if (lines < 0) terminal->viewport_offset = amount > offset ? 0u : offset - amount;
	moved =
		terminal->viewport_offset > offset ? terminal->viewport_offset - offset : offset - terminal->viewport_offset;
	if (terminal->viewport_dirty) success = redraw_viewport(terminal);
	else success = moved == 0u || move_viewport(terminal, terminal->viewport_offset > offset, moved);
	success = success && draw_overlay(terminal) && show_cursor(terminal);
	terminal_unlock(terminal);
	return success;
}
