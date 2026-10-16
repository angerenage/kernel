#pragma once

/*
 * UTF-8 framebuffer terminal output protocol.
 *
 * The single system terminal accepts the generic Stream protocol's write
 * operation. Text is UTF-8, with LF line breaks, CR, BS, and four-column
 * tabs. It recognizes ANSI SGR reset, normal and bright 16-color foreground
 * and background selections, default colors, bold-color selection, underline,
 * reverse video, and the equivalent 38;5/48;5 palette selections. Standard
 * CSI cursor movement and positioning, save/restore, line/display erasure,
 * and DEC ?25 cursor visibility sequences are also supported. The cursor is a
 * steady block that is hidden while a write is applied and redrawn afterward.
 * Other escape sequences are ignored.
 *
 * Current specification version: 1.0-beta
 */

#include <base/cap.h>

/* Registry identity of the single framebuffer terminal. */
#define TERMINAL_NAMESPACE "io"
#define TERMINAL_PROTOCOL_NAME "terminal"
#define TERMINAL_SERVICE_NAME "default"

/* Exact rights granted to terminal writers. */
#define TERMINAL_CLIENT_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_WRITE | CAP_DELEGATE))
