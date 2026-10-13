#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoder state for one keyboard speaking untranslated scan-code set 2. */
struct ps2_keyboard_decoder {
	bool   extended;
	bool   released;
	bool   pause;
	size_t pause_index;
};

/* Reset all partial prefixes and multi-byte sequences. */
void ps2_keyboard_decoder_init(struct ps2_keyboard_decoder* decoder);

/* Decode one byte, returning false only for an irrecoverable malformed sequence. */
bool ps2_keyboard_decoder_feed(struct ps2_keyboard_decoder* decoder, uint8_t value);
