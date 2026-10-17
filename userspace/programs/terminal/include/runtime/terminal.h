#pragma once

#include <base/cap.h>
#include <base/syscall.h>
#include <protocol/terminal.h>

/* Read the framebuffer terminal's immutable character-cell dimensions. */
syscall_status_t terminal_get_info(cap_id_t terminal_cap, struct terminal_get_info_response* out_info);
