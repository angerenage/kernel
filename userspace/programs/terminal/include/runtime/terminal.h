#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/terminal.h>

/* Read the framebuffer terminal's immutable character-cell dimensions. */
syscall_status_t terminal_get_info(cap_id_t terminal_cap, struct terminal_get_info_response* out_info);

/* Read the framebuffer terminal's current character-cell cursor position. */
syscall_status_t terminal_get_cursor(cap_id_t terminal_cap, struct terminal_get_cursor_response* out_cursor);

/* Acquire the terminal's coalesced interrupt notification signal. */
syscall_status_t terminal_acquire_interrupt_signal(cap_id_t terminal_cap, cap_id_t* out_signal_cap);

/* Discard buffered input and return the interrupt generation at that boundary. */
syscall_status_t terminal_discard_input(cap_id_t terminal_cap, uint64_t* out_interrupt_generation);
