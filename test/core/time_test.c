#define sched_yield host_sched_yield
#include <pthread.h>
#undef sched_yield

#include <core/cpu.h>
#include <core/sched.h>
#include <core/time.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>

#include "../mocks/hal/clock.h"

static void time_test_init_scheduler(void) {
	irq_enable_local();
	cr_assert(cpu_topology_init_bootstrap(0x100000u, 0x104000u));
	cr_assert_not_null(cpu_bsp());
	cpu_bind_current(cpu_bsp());
	cpu_interrupts_set_ready(cpu_current(), false);
	cr_assert(sched_init());
	cr_assert(sched_start_cpu(cpu_current()));
}

#define TIME_TEST_THREAD_COUNT 4u
#define TIME_TEST_THREAD_READS 1000u

static void* time_test_concurrent_reader(void* arg) {
	bool* failed = arg;

	for (size_t index = 0u; index < TIME_TEST_THREAD_READS; index++) {
		uint64_t now;

		(void)hal_clock_mock_advance_monotonic_counter(1u);
		if (!time_monotonic_now(&now)) __atomic_store_n(failed, true, __ATOMIC_RELEASE);
	}
	return NULL;
}

Test(core_time, hardware_counter_uses_an_arbitrary_zero_origin_and_non_round_frequency) {
	uint64_t now = UINT64_MAX;

	hal_clock_init();
	hal_clock_mock_set_monotonic(3u, 10u, true);
	cr_assert(time_monotonic_init());
	cr_assert_eq(time_monotonic_resolution_ns(), 333333334u);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 0u);

	hal_clock_mock_set_monotonic_counter(12u);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 666666666u);
}

Test(core_time, regressing_hardware_samples_are_clamped) {
	uint64_t now;

	hal_clock_init();
	hal_clock_mock_set_monotonic(1000u, 100u, true);
	cr_assert(time_monotonic_init());
	hal_clock_mock_set_monotonic_counter(110u);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 10000000u);

	hal_clock_mock_set_monotonic_counter(105u);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 10000000u);
}

Test(core_time, concurrent_observations_preserve_the_high_water_mark) {
	pthread_t threads[TIME_TEST_THREAD_COUNT];
	bool      failed = false;
	uint64_t  now;

	hal_clock_init();
	hal_clock_mock_set_monotonic(1000000000u, 0u, true);
	cr_assert(time_monotonic_init());
	for (size_t index = 0u; index < TIME_TEST_THREAD_COUNT; index++)
		cr_assert_eq(pthread_create(&threads[index], NULL, time_test_concurrent_reader, &failed), 0);
	for (size_t index = 0u; index < TIME_TEST_THREAD_COUNT; index++)
		cr_assert_eq(pthread_join(threads[index], NULL), 0);
	cr_assert_not(__atomic_load_n(&failed, __ATOMIC_ACQUIRE));
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, TIME_TEST_THREAD_COUNT * TIME_TEST_THREAD_READS);
}

Test(core_time, scheduler_ticks_are_used_as_the_fallback) {
	uint64_t now;

	hal_clock_init();
	hal_clock_mock_set_monotonic(1000000000u, 0u, false);
	time_test_init_scheduler();
	cr_assert(hal_clock_start(100u, NULL, NULL));
	cr_assert(time_monotonic_init());
	cr_assert_eq(time_monotonic_resolution_ns(), 10000000u);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 0u);

	sched_tick();
	sched_tick();
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, 20000000u);
}

Test(core_time, unavailable_sources_and_invalid_output_are_rejected) {
	uint64_t now = 42u;

	hal_clock_init();
	cr_assert_not(time_monotonic_init());
	cr_assert_eq(time_monotonic_resolution_ns(), 0u);
	cr_assert_not(time_monotonic_now(&now));
	cr_assert_eq(now, 42u);
	cr_assert_not(time_monotonic_now(NULL));
}

Test(core_time, nanosecond_conversion_saturates) {
	uint64_t now;

	hal_clock_init();
	hal_clock_mock_set_monotonic(1u, 0u, true);
	cr_assert(time_monotonic_init());
	hal_clock_mock_set_monotonic_counter(UINT64_MAX);
	cr_assert(time_monotonic_now(&now));
	cr_assert_eq(now, UINT64_MAX);
}
