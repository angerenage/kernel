#include <hal/clock.h>

static uint32_t mock_frequency_hz;

void hal_clock_init(void) {
	mock_frequency_hz = 0u;
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	(void)handler;
	(void)ctx;
	mock_frequency_hz = frequency_hz;
	return frequency_hz != 0u;
}

uint32_t hal_clock_frequency(void) {
	return mock_frequency_hz;
}

void hal_clock_stop(void) {
	mock_frequency_hz = 0u;
}
