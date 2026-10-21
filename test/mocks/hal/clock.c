#include <hal/clock.h>
#include <stddef.h>

static uint32_t mock_frequency_hz;
static uint64_t mock_monotonic_frequency_hz;
static uint64_t mock_monotonic_counter;
static bool     mock_monotonic_readable;

void hal_clock_init(void) {
	mock_frequency_hz           = 0u;
	mock_monotonic_frequency_hz = 0u;
	mock_monotonic_counter      = 0u;
	mock_monotonic_readable     = false;
}

uint64_t hal_clock_monotonic_frequency(void) {
	return __atomic_load_n(&mock_monotonic_frequency_hz, __ATOMIC_ACQUIRE);
}

bool hal_clock_monotonic_counter(uint64_t* out_counter) {
	if (out_counter == NULL || !__atomic_load_n(&mock_monotonic_readable, __ATOMIC_ACQUIRE) ||
	    hal_clock_monotonic_frequency() == 0u)
		return false;
	*out_counter = __atomic_load_n(&mock_monotonic_counter, __ATOMIC_ACQUIRE);
	return true;
}

void hal_clock_mock_set_monotonic(uint64_t frequency_hz, uint64_t counter, bool readable) {
	__atomic_store_n(&mock_monotonic_frequency_hz, frequency_hz, __ATOMIC_RELEASE);
	__atomic_store_n(&mock_monotonic_counter, counter, __ATOMIC_RELEASE);
	__atomic_store_n(&mock_monotonic_readable, readable, __ATOMIC_RELEASE);
}

void hal_clock_mock_set_monotonic_counter(uint64_t counter) {
	__atomic_store_n(&mock_monotonic_counter, counter, __ATOMIC_RELEASE);
}

uint64_t hal_clock_mock_advance_monotonic_counter(uint64_t delta) {
	return __atomic_add_fetch(&mock_monotonic_counter, delta, __ATOMIC_ACQ_REL);
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
