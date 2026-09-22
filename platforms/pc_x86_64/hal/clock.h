#pragma once

#include <stdbool.h>

/* Service a legacy timer interrupt when the vector belongs to the clock. */
bool clock_handle_irq(unsigned vector);
