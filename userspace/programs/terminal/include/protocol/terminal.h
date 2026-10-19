#pragma once

/*
 * UTF-8 framebuffer terminal byte-stream protocol.
 *
 * The single system terminal uses the generic Stream protocol. Reads block
 * until keyboard input is available and return committed UTF-8 text or
 * conventional terminal control bytes and escape sequences. The managing TTY
 * receives the interrupt character through a separate coalesced signal so it
 * remains observable while ordinary input is backpressured. Writes render UTF-8,
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
 * dimensions and its current cursor position. Selection operations use stable,
 * monotonically assigned line identities and half-open cell ranges; anchor and
 * focus order is preserved so clients can extend either end of a selection.
 * The terminal also owns a bounded, length-delimited UTF-8 clipboard. Clipboard
 * text is plain text: terminal styling is never represented in its contents.
 * Tab, LF, and CR are the only permitted control characters; NUL, other C0,
 * DEL, and C1 controls are rejected.
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

/* Rights retained by the TTY session for input routing and selection forwarding. */
#define TERMINAL_TTY_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_WRITE | CAP_MANAGE))

/* Rights granted on the terminal interrupt notification signal. */
#define TERMINAL_INTERRUPT_SIGNAL_RIGHTS ((cap_rights_t)CAP_WAIT)

/* Terminal-specific operations following the generic Stream operations. */
enum terminal_op {
	TERMINAL_OP_GET_INFO = 2u,
	TERMINAL_OP_GET_CURSOR,
	TERMINAL_OP_ACQUIRE_INTERRUPT_SIGNAL,
	TERMINAL_OP_DISCARD_INPUT,
	TERMINAL_OP_GET_SELECTION,
	TERMINAL_OP_SET_SELECTION,
	TERMINAL_OP_CLEAR_SELECTION,
	TERMINAL_OP_GET_CLIPBOARD,
	TERMINAL_OP_SET_CLIPBOARD,
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

/* Request the current terminal cursor position. */
struct terminal_get_cursor_request {
	struct terminal_request_header header;
};

/* Current cursor position measured in zero-based character cells. */
struct terminal_get_cursor_response {
	uint64_t column;
	uint64_t row;
	uint64_t line;
};

/* One cell boundary on a stable retained terminal line. */
struct terminal_selection_position {
	uint64_t line;
	uint64_t column;
};

/* Request the currently active selection. */
struct terminal_get_selection_request {
	struct terminal_request_header header;
};

/* Directional endpoints of the currently active half-open selection. */
struct terminal_get_selection_response {
	struct terminal_selection_position anchor;
	struct terminal_selection_position focus;
};

/* Replace the selection with one nonempty retained cell range. */
struct terminal_set_selection_request {
	struct terminal_request_header     header;
	struct terminal_selection_position anchor;
	struct terminal_selection_position focus;
};

/* Clear the current selection, succeeding when none is active. */
struct terminal_clear_selection_request {
	struct terminal_request_header header;
};

/* Request the terminal-owned plain-text clipboard. */
struct terminal_get_clipboard_request {
	struct terminal_request_header header;
};

/* Clipboard metadata followed by size bytes of UTF-8 text. */
struct terminal_get_clipboard_response {
	uint32_t size;
	uint32_t reserved;
	uint8_t  text[];
};

/* Replace the clipboard with the size bytes of UTF-8 text that follow. */
struct terminal_set_clipboard_request {
	struct terminal_request_header header;
	uint32_t                       size;
	uint32_t                       reserved;
	uint8_t                        text[];
};

/* Largest clipboard value that can be replaced atomically. */
#define TERMINAL_CLIPBOARD_CAPACITY (CAP_MAX_REQUEST_SIZE - sizeof(struct terminal_set_clipboard_request))

/* Acquire notification of interrupt characters independently of buffered input. */
struct terminal_acquire_interrupt_signal_request {
	struct terminal_request_header header;
};

/* Capability for the coalesced interrupt notification signal. */
struct terminal_acquire_interrupt_signal_response {
	cap_id_t capability;
};

/* Discard buffered terminal input before changing its recipient. */
struct terminal_discard_input_request {
	struct terminal_request_header header;
};

/* Interrupt generation current at the input-discard boundary. */
struct terminal_discard_input_response {
	uint64_t interrupt_generation;
};
