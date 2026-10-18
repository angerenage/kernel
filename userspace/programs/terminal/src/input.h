#pragma once

#include <base/cap.h>
#include <base/process.h>
#include <stdbool.h>
#include <stdint.h>

struct terminal;
struct terminal_input;

/* Allocate terminal input state bound to one renderer. */
struct terminal_input* terminal_input_create(struct terminal* terminal);

/* Start the text-input consumer. */
bool terminal_input_start(struct terminal_input* input);

/* Cancel the consumers, fail pending reads, and release input state. */
void terminal_input_destroy(struct terminal_input* input);

/* Validate and either complete or retain one blocking Stream read. */
bool terminal_input_read(struct terminal_input* input, const struct cap_request* call, const uint8_t* request_data);

/* Grant one caller access to out-of-band interrupt notifications. */
bool terminal_input_acquire_interrupt_signal(struct terminal_input* input, process_id_t caller,
                                             cap_id_t* out_signal_cap);

/* Discard unread bytes and return the current interrupt generation. */
uint64_t terminal_input_discard(struct terminal_input* input);
