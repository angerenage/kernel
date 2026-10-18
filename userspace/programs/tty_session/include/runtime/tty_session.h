#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <protocol/tty_session.h>
#include <runtime/init.h>

/* Acquire the single system TTY session. */
struct init_call_result tty_session_acquire(struct init_service_handle* out_handle);

/* Read the character-cell dimensions of the terminal behind a TTY session. */
syscall_status_t tty_session_get_info(cap_id_t session_cap, struct tty_session_get_info_response* out_info);

/* Read the current character-cell cursor position behind a TTY session. */
syscall_status_t tty_session_get_cursor(cap_id_t session_cap, struct tty_session_get_cursor_response* out_cursor);

/* Claim foreground-process control for the calling shell. */
syscall_status_t tty_session_claim(cap_id_t session_cap);

/* Select a prepared child process before starting it. */
syscall_status_t tty_session_begin_foreground(cap_id_t session_cap, process_id_t process_id);

/* Delegate control of the selected child after it has started. */
syscall_status_t tty_session_attach_foreground(cap_id_t session_cap, process_id_t session_owner, cap_id_t process_cap,
                                               process_id_t process_id);

/* Restore the controlling shell as the foreground recipient. */
syscall_status_t tty_session_restore_foreground(cap_id_t session_cap);
