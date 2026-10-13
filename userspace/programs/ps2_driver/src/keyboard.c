#include "keyboard.h"

#include "events.h"

void ps2_keyboard_decoder_init(struct ps2_keyboard_decoder* decoder) {
	*decoder = (struct ps2_keyboard_decoder){0};
}

bool ps2_keyboard_decoder_feed(struct ps2_keyboard_decoder* decoder, uint8_t value) {
	static const uint8_t pause_tail[] = {0x14u, 0x77u, 0xe1u, 0xf0u, 0x14u, 0xf0u, 0x77u};

	if (decoder->pause) {
		if (value != pause_tail[decoder->pause_index]) {
			ps2_keyboard_decoder_init(decoder);
			return false;
		}
		decoder->pause_index++;
		if (decoder->pause_index == sizeof(pause_tail)) {
			if (!ps2_event_pause()) return false;
			ps2_keyboard_decoder_init(decoder);
		}
		return true;
	}
	if (value == 0xe1u) {
		decoder->pause       = true;
		decoder->pause_index = 0u;
		decoder->extended    = false;
		decoder->released    = false;
		return true;
	}
	if (value == 0xe0u) {
		decoder->extended = true;
		return true;
	}
	if (value == 0xf0u) {
		decoder->released = true;
		return true;
	}
	/* Print Screen wraps its actual E0:7c code in synthetic E0:12 transitions. */
	if (decoder->extended && value == 0x12u) {
		decoder->extended = false;
		decoder->released = false;
		return true;
	}
	if (!ps2_event_key(value, decoder->extended, !decoder->released)) return false;
	decoder->extended = false;
	decoder->released = false;
	return true;
}
