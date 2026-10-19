#pragma once

/*
 * Foreground process and terminal input session protocol.
 *
 * The single system TTY session presents a generic readable Stream. A shell
 * receives a managing capability from init and selects one foreground child at
 * a time. Input is delivered only to the foreground process; Control-C
 * terminates a selected child and is otherwise delivered to the shell as an
 * ordinary control byte.
 * Terminal geometry, cursor, selection, and plain-text clipboard operations
 * are forwarded without exposing the underlying terminal capability to
 * clients.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <base/process.h>
#include <stdint.h>

/* Registry identity of the single system TTY session. */
#define TTY_SESSION_NAMESPACE "io"
#define TTY_SESSION_PROTOCOL_NAME "tty"
#define TTY_SESSION_SERVICE_NAME "default"

/* Rights granted to ordinary clients through the init registry. */
#define TTY_SESSION_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))

/* Rights installed by init on the controlling shell's standard input. */
#define TTY_SESSION_CONTROL_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Rights delegated when the TTY session is used only as standard input. */
#define TTY_SESSION_INPUT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))

/* TTY-specific operations following the generic Stream operations. */
enum tty_session_op {
	TTY_SESSION_OP_GET_INFO = 2u,
	TTY_SESSION_OP_GET_CURSOR,
	TTY_SESSION_OP_BEGIN_FOREGROUND,
	TTY_SESSION_OP_ATTACH_FOREGROUND,
	TTY_SESSION_OP_RESTORE_FOREGROUND,
	TTY_SESSION_OP_GET_SELECTION,
	TTY_SESSION_OP_SET_SELECTION,
	TTY_SESSION_OP_CLEAR_SELECTION,
	TTY_SESSION_OP_GET_CLIPBOARD,
	TTY_SESSION_OP_SET_CLIPBOARD,
};

/* Common header for TTY-specific requests. */
struct tty_session_request_header {
	enum tty_session_op op;
};

/* Request the underlying terminal dimensions. */
struct tty_session_get_info_request {
	struct tty_session_request_header header;
};

/* Terminal dimensions measured in fixed-width character cells. */
struct tty_session_get_info_response {
	uint64_t     columns;
	uint64_t     rows;
	process_id_t session_pid;
};

/* Request the current terminal cursor position. */
struct tty_session_get_cursor_request {
	struct tty_session_request_header header;
};

/* Current cursor position measured in zero-based character cells. */
struct tty_session_get_cursor_response {
	uint64_t column;
	uint64_t row;
	uint64_t line;
};

/* One cell boundary on a stable retained terminal line. */
struct tty_session_selection_position {
	uint64_t line;
	uint64_t column;
};

/* Request the currently active terminal selection. */
struct tty_session_get_selection_request {
	struct tty_session_request_header header;
};

/* Directional endpoints of the currently active half-open selection. */
struct tty_session_get_selection_response {
	struct tty_session_selection_position anchor;
	struct tty_session_selection_position focus;
};

/* Replace the terminal selection with one nonempty half-open cell range. */
struct tty_session_set_selection_request {
	struct tty_session_request_header     header;
	struct tty_session_selection_position anchor;
	struct tty_session_selection_position focus;
};

/* Clear the terminal selection, succeeding when none is active. */
struct tty_session_clear_selection_request {
	struct tty_session_request_header header;
};

/* Request the terminal-owned plain-text clipboard. */
struct tty_session_get_clipboard_request {
	struct tty_session_request_header header;
};

/* Clipboard metadata followed by size bytes of UTF-8 text. */
struct tty_session_get_clipboard_response {
	uint32_t size;
	uint32_t reserved;
	uint8_t  text[];
};

/* Replace the clipboard with the size bytes of UTF-8 text that follow. */
struct tty_session_set_clipboard_request {
	struct tty_session_request_header header;
	uint32_t                          size;
	uint32_t                          reserved;
	uint8_t                           text[];
};

/* Largest clipboard value that can be replaced atomically. */
#define TTY_SESSION_CLIPBOARD_CAPACITY (CAP_MAX_REQUEST_SIZE - sizeof(struct tty_session_set_clipboard_request))

/* Select a prepared child process as the foreground input recipient. */
struct tty_session_begin_foreground_request {
	struct tty_session_request_header header;
	process_id_t                      process_id;
};

/*
 * Supply the process-control capability after the foreground child starts.
 * A successful request transfers ownership of process_cap to the session.
 */
struct tty_session_attach_foreground_request {
	struct tty_session_request_header header;
	cap_id_t                          process_cap;
	process_id_t                      process_id;
};

/* Restore the controlling shell as the foreground input recipient. */
struct tty_session_restore_foreground_request {
	struct tty_session_request_header header;
};
