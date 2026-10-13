#include "mouse.h"

#include "events.h"

#define PS2_MOUSE_PACKET_SYNC (1u << 3u)
#define PS2_MOUSE_X_OVERFLOW (1u << 6u)
#define PS2_MOUSE_Y_OVERFLOW (1u << 7u)
#define PS2_MOUSE_RESYNC_LIMIT 8u

void ps2_mouse_decoder_init(struct ps2_mouse_decoder* decoder, uint8_t device_id) {
	*decoder             = (struct ps2_mouse_decoder){0};
	decoder->packet_size = device_id == 0x03u || device_id == 0x04u ? 4u : 3u;
}

bool ps2_mouse_decoder_feed(struct ps2_mouse_decoder* decoder, uint8_t value) {
	if (decoder->packet_index == 0u && (value & PS2_MOUSE_PACKET_SYNC) == 0u) {
		decoder->invalid_starts++;
		return decoder->invalid_starts < PS2_MOUSE_RESYNC_LIMIT;
	}
	decoder->invalid_starts                  = 0u;
	decoder->packet[decoder->packet_index++] = value;
	if (decoder->packet_index != decoder->packet_size) return true;
	decoder->packet_index = 0u;
	if ((decoder->packet[0] & (PS2_MOUSE_X_OVERFLOW | PS2_MOUSE_Y_OVERFLOW)) != 0u) return true;

	int16_t delta_x = (int8_t)decoder->packet[1];
	int16_t delta_y = -(int16_t)(int8_t)decoder->packet[2];
	int8_t  wheel   = 0;
	uint8_t buttons = decoder->packet[0] & 0x07u;
	if (decoder->packet_size == 4u) {
		wheel = (int8_t)(decoder->packet[3] & 0x0fu);
		if ((wheel & 0x08) != 0) wheel = (int8_t)(wheel | (int8_t)0xf0);
		if ((decoder->packet[3] & (1u << 4u)) != 0u) buttons |= 1u << 3u;
		if ((decoder->packet[3] & (1u << 5u)) != 0u) buttons |= 1u << 4u;
	}
	return ps2_event_mouse(delta_x, delta_y, wheel, buttons);
}
