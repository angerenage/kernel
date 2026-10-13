#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoder state for one standard, wheel, or five-button PS/2 mouse. */
struct ps2_mouse_decoder {
	uint8_t packet[4];
	size_t  packet_size;
	size_t  packet_index;
	size_t  invalid_starts;
};

/* Reset packet synchronization for the identified mouse protocol. */
void ps2_mouse_decoder_init(struct ps2_mouse_decoder* decoder, uint8_t device_id);

/* Decode one byte, returning false after repeated loss of packet synchronization. */
bool ps2_mouse_decoder_feed(struct ps2_mouse_decoder* decoder, uint8_t value);
