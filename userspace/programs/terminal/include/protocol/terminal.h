#pragma once

/*
 * UTF-8 framebuffer terminal byte-stream protocol.
 *
 * The single system terminal uses the generic Stream protocol. Reads block
 * until keyboard input is available and return committed UTF-8 text or
 * conventional terminal control bytes and escape sequences. Writes render UTF-8,
 * with LF line breaks, CR, BS, and four-column tabs. The renderer recognizes
 * ANSI SGR reset, normal and bright 16-color foreground and background selections,
 * default colors, bold-color selection, underline, reverse video,
 * and the equivalent 38;5/48;5 palette selections. Standard CSI cursor
 * movement and positioning, save/restore, line/display erasure,
 * and DEC ?25 cursor visibility sequences are also supported. The cursor
 * is a steady block that is hidden while a write is applied and redrawn
 * afterward. Other escape sequences are ignored.
 *
 * Besides the Stream operations, clients may query the immutable terminal
 * dimensions.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>
#include <stdint.h>

/* Registry identity of the single framebuffer terminal. */
#define TERMINAL_NAMESPACE "io"
#define TERMINAL_PROTOCOL_NAME "terminal"
#define TERMINAL_SERVICE_NAME "default"

/* Maximum rights granted when the terminal service is acquired. */
#define TERMINAL_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_DELEGATE))

/* Rights required by a process that only consumes terminal input. */
#define TERMINAL_INPUT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ))

/* Rights required by a process that only renders terminal output. */
#define TERMINAL_OUTPUT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_WRITE))

/* Terminal-specific operations following the generic Stream operations. */
enum terminal_op {
	TERMINAL_OP_GET_INFO = 2u,
};

/* Common header for terminal-specific requests. */
struct terminal_request_header {
	enum terminal_op op;
};

/* Request immutable terminal dimensions. */
struct terminal_get_info_request {
	struct terminal_request_header header;
};

/* Terminal dimensions measured in fixed-width character cells. */
struct terminal_get_info_response {
	uint64_t columns;
	uint64_t rows;
};
