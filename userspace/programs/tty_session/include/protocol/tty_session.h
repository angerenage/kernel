#pragma once

/*
 * Foreground process and terminal input session protocol.
 *
 * The single system TTY session presents a generic readable Stream. A shell
 * claims the session and selects one foreground child at a time. Input is
 * delivered only to the foreground process; Control-C terminates a selected
 * child and is otherwise delivered to the shell as an ordinary control byte.
 * Terminal geometry and cursor queries are forwarded without exposing the
 * underlying terminal capability to clients.
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

/* Maximum rights granted when the TTY session is acquired. */
#define TTY_SESSION_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))

/* Rights delegated when the TTY session is used only as standard input. */
#define TTY_SESSION_INPUT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))

/* TTY-specific operations following the generic Stream operations. */
enum tty_session_op {
	TTY_SESSION_OP_GET_INFO = 2u,
	TTY_SESSION_OP_GET_CURSOR,
	TTY_SESSION_OP_CLAIM,
	TTY_SESSION_OP_BEGIN_FOREGROUND,
	TTY_SESSION_OP_ATTACH_FOREGROUND,
	TTY_SESSION_OP_RESTORE_FOREGROUND,
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
	uint64_t columns;
	uint64_t rows;
};

/* Request the current terminal cursor position. */
struct tty_session_get_cursor_request {
	struct tty_session_request_header header;
};

/* Current cursor position measured in zero-based character cells. */
struct tty_session_get_cursor_response {
	uint64_t column;
	uint64_t row;
};

/* Claim responsibility for foreground selection for the calling shell. */
struct tty_session_claim_request {
	struct tty_session_request_header header;
};

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
