#include <core/sched.h>
#include <core/time.h>
#include <hal/clock.h>
#include <stddef.h>
#include <stdint.h>

#define NANOSECONDS_PER_SECOND 1000000000ull

enum monotonic_source {
	MONOTONIC_SOURCE_NONE = 0,
	MONOTONIC_SOURCE_HARDWARE,
	MONOTONIC_SOURCE_SCHEDULER,
};

static uint32_t monotonic_source;
static uint64_t monotonic_origin;
static uint64_t monotonic_frequency;
static uint64_t monotonic_last_ns;

static uint64_t monotonic_resolution(uint64_t frequency) {
	uint64_t resolution;

	if (frequency == 0u) return 0u;
	resolution = NANOSECONDS_PER_SECOND / frequency;
	if (NANOSECONDS_PER_SECOND % frequency != 0u) resolution++;
	return resolution == 0u ? 1u : resolution;
}

static uint64_t monotonic_fractional_ns(uint64_t remainder, uint64_t frequency) {
	uint64_t quotient = 0u;
	uint64_t residue  = 0u;

	/* Long division computes remainder * 1e9 / frequency without overflowing either product. */
	for (unsigned bit = 30u; bit-- > 0u;) {
		quotient *= 2u;
		if (residue >= frequency - residue) {
			residue -= frequency - residue;
			quotient++;
		}
		else {
			residue *= 2u;
		}
		if ((NANOSECONDS_PER_SECOND & (1ull << bit)) == 0u) continue;
		if (residue >= frequency - remainder) {
			residue -= frequency - remainder;
			quotient++;
		}
		else {
			residue += remainder;
		}
	}
	return quotient;
}

static uint64_t monotonic_ticks_to_ns(uint64_t ticks, uint64_t frequency) {
	uint64_t seconds;
	uint64_t nanoseconds;
	uint64_t fractional;

	if (frequency == 0u) return UINT64_MAX;
	seconds = ticks / frequency;
	if (seconds > UINT64_MAX / NANOSECONDS_PER_SECOND) return UINT64_MAX;
	nanoseconds = seconds * NANOSECONDS_PER_SECOND;
	fractional  = monotonic_fractional_ns(ticks % frequency, frequency);
	if (fractional > UINT64_MAX - nanoseconds) return UINT64_MAX;
	return nanoseconds + fractional;
}

bool time_monotonic_init(void) {
	uint64_t frequency;
	uint64_t origin;

	__atomic_store_n(&monotonic_source, MONOTONIC_SOURCE_NONE, __ATOMIC_RELEASE);
	__atomic_store_n(&monotonic_last_ns, 0u, __ATOMIC_RELEASE);

	frequency = hal_clock_monotonic_frequency();
	if (frequency != 0u && hal_clock_monotonic_counter(&origin)) {
		monotonic_origin    = origin;
		monotonic_frequency = frequency;
		__atomic_store_n(&monotonic_source, MONOTONIC_SOURCE_HARDWARE, __ATOMIC_RELEASE);
		return true;
	}

	frequency = hal_clock_frequency();
	if (frequency == 0u) return false;
	monotonic_origin    = sched_tick_count();
	monotonic_frequency = frequency;
	__atomic_store_n(&monotonic_source, MONOTONIC_SOURCE_SCHEDULER, __ATOMIC_RELEASE);
	return true;
}

bool time_monotonic_now(uint64_t* out_nanoseconds) {
	uint32_t source;
	uint64_t counter;
	uint64_t elapsed;
	uint64_t candidate;
	uint64_t previous;

	if (out_nanoseconds == NULL) return false;
	source = __atomic_load_n(&monotonic_source, __ATOMIC_ACQUIRE);
	if (source == MONOTONIC_SOURCE_HARDWARE) {
		if (!hal_clock_monotonic_counter(&counter)) return false;
	}
	else if (source == MONOTONIC_SOURCE_SCHEDULER) {
		counter = sched_tick_count();
	}
	else {
		return false;
	}

	elapsed   = counter < monotonic_origin ? 0u : counter - monotonic_origin;
	candidate = monotonic_ticks_to_ns(elapsed, monotonic_frequency);
	previous  = __atomic_load_n(&monotonic_last_ns, __ATOMIC_ACQUIRE);
	while (candidate > previous &&
	       !__atomic_compare_exchange_n(
			   &monotonic_last_ns, &previous, candidate, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
	}
	*out_nanoseconds = candidate > previous ? candidate : previous;
	return true;
}

uint64_t time_monotonic_resolution_ns(void) {
	if (__atomic_load_n(&monotonic_source, __ATOMIC_ACQUIRE) == MONOTONIC_SOURCE_NONE) return 0u;
	return monotonic_resolution(monotonic_frequency);
}
