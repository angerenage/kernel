#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <protocol/tty_session.h>
#include <runtime/registry.h>
#include <stddef.h>

/* Acquire the single system TTY session. */
struct registry_call_result tty_session_acquire(struct registry_service_handle* out_handle);

/* Read the character-cell dimensions of the terminal behind a TTY session. */
syscall_status_t tty_session_get_info(cap_id_t session_cap, struct tty_session_get_info_response* out_info);

/* Read the current character-cell cursor position behind a TTY session. */
syscall_status_t tty_session_get_cursor(cap_id_t session_cap, struct tty_session_get_cursor_response* out_cursor);

/* Read the directional endpoints of the active terminal selection. */
syscall_status_t tty_session_get_selection(cap_id_t                                   session_cap,
                                           struct tty_session_get_selection_response* out_selection);

/* Replace the terminal selection with one nonempty retained cell range. */
syscall_status_t tty_session_set_selection(cap_id_t session_cap, struct tty_session_selection_position anchor,
                                           struct tty_session_selection_position focus);

/* Clear the terminal selection. */
syscall_status_t tty_session_clear_selection(cap_id_t session_cap);

/* Read the terminal-owned plain-text clipboard. */
syscall_status_t tty_session_get_clipboard(cap_id_t session_cap, void* buffer, size_t capacity, size_t* out_size);

/* Replace the terminal-owned plain-text clipboard. */
syscall_status_t tty_session_set_clipboard(cap_id_t session_cap, const void* text, size_t size);

/* Select a prepared child process before starting it. */
syscall_status_t tty_session_begin_foreground(cap_id_t session_cap, process_id_t process_id);

/* Delegate control of the selected child after it has started. */
syscall_status_t tty_session_attach_foreground(cap_id_t session_cap, process_id_t session_owner, cap_id_t process_cap,
                                               process_id_t process_id);

/* Restore the controlling shell as the foreground recipient. */
syscall_status_t tty_session_restore_foreground(cap_id_t session_cap);
