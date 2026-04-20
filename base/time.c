#include <base/math.h>
#include <base/time.h>

bool time_ms_to_ticks(uint64_t ms, uint32_t hz, uint64_t* out_ticks) {
	uint64_t ticks;

	if (out_ticks == NULL || hz == 0u) return false;

	if (ms == 0u) {
		*out_ticks = 0u;
		return true;
	}

	if (mul_overflow_u64(ms, (uint64_t)hz, &ticks) || add_overflow_u64(ticks, 999u, &ticks)) {
		ticks = UINT64_MAX;
	}
	else {
		ticks /= 1000u;
		if (ticks == 0u) ticks = 1u;
	}

	*out_ticks = ticks;
	return true;
}

bool time_tick_deadline_from_ms(uint64_t current_tick, uint64_t timeout_ms, uint32_t hz, uint64_t* out_deadline_tick) {
	uint64_t deadline_tick;
	uint64_t sleep_ticks;

	if (out_deadline_tick == NULL || timeout_ms == 0u) return false;
	if (!time_ms_to_ticks(timeout_ms, hz, &sleep_ticks)) return false;
	if (sleep_ticks == 0u) return false;

	if (add_overflow_u64(current_tick, sleep_ticks, &deadline_tick)) return false;

	*out_deadline_tick = deadline_tick;
	return true;
}
