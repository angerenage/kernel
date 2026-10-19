#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/terminal.h>

/* Read the framebuffer terminal's immutable character-cell dimensions. */
syscall_status_t terminal_get_info(cap_id_t terminal_cap, struct terminal_get_info_response* out_info);

/* Read the framebuffer terminal's current character-cell cursor position. */
syscall_status_t terminal_get_cursor(cap_id_t terminal_cap, struct terminal_get_cursor_response* out_cursor);

/* Read the active directional selection. */
syscall_status_t terminal_get_selection(cap_id_t terminal_cap, struct terminal_get_selection_response* out_selection);

/* Replace the active selection with one retained half-open cell range. */
syscall_status_t terminal_set_selection(cap_id_t terminal_cap, struct terminal_selection_position anchor,
                                        struct terminal_selection_position focus);

/* Clear the active selection. */
syscall_status_t terminal_clear_selection(cap_id_t terminal_cap);

/* Read the clipboard, or query its byte size with a null buffer and zero capacity. */
syscall_status_t terminal_get_clipboard(cap_id_t terminal_cap, void* buffer, size_t capacity, size_t* out_size);

/* Atomically replace the terminal-owned plain-text clipboard. */
syscall_status_t terminal_set_clipboard(cap_id_t terminal_cap, const void* text, size_t size);

/* Acquire the terminal's coalesced interrupt notification signal. */
syscall_status_t terminal_acquire_interrupt_signal(cap_id_t terminal_cap, cap_id_t* out_signal_cap);

/* Discard buffered input and return the interrupt generation at that boundary. */
syscall_status_t terminal_discard_input(cap_id_t terminal_cap, uint64_t* out_interrupt_generation);
