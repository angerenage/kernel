#pragma once

#include <stdbool.h>
#include <stdint.h>

void     hal_clock_mock_set_monotonic(uint64_t frequency_hz, uint64_t counter, bool readable);
void     hal_clock_mock_set_monotonic_counter(uint64_t counter);
uint64_t hal_clock_mock_advance_monotonic_counter(uint64_t delta);
