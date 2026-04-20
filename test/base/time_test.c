#include <base/time.h>
#include <criterion/criterion.h>
#include <stdint.h>

Test(base_time, ms_to_ticks_rounds_up_partial_ticks) {
	uint64_t ticks = UINT64_MAX;

	cr_assert(time_ms_to_ticks(1u, 100u, &ticks));
	cr_assert_eq(ticks, 1u);

	cr_assert(time_ms_to_ticks(11u, 100u, &ticks));
	cr_assert_eq(ticks, 2u);
}

Test(base_time, ms_to_ticks_handles_zero_and_saturates_on_overflow) {
	uint64_t ticks = 123u;

	cr_assert(time_ms_to_ticks(0u, 100u, &ticks));
	cr_assert_eq(ticks, 0u);

	cr_assert(time_ms_to_ticks(UINT64_MAX, UINT32_MAX, &ticks));
	cr_assert_eq(ticks, UINT64_MAX);
}

Test(base_time, deadline_from_ms_adds_relative_timeout_to_current_tick) {
	uint64_t deadline = 0u;

	cr_assert(time_tick_deadline_from_ms(40u, 25u, 100u, &deadline));
	cr_assert_eq(deadline, 43u);
}

Test(base_time, deadline_from_ms_rejects_invalid_inputs_and_tick_overflow) {
	uint64_t deadline = 77u;

	cr_assert_not(time_tick_deadline_from_ms(10u, 0u, 100u, &deadline));
	cr_assert_not(time_tick_deadline_from_ms(UINT64_MAX, 1u, 1000u, &deadline));
	cr_assert_eq(deadline, 77u);
}
