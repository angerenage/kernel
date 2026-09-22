#pragma once

#include <stdbool.h>

#include "interrupts/frame.h"

/* Service a processor timer interrupt when one is pending. */
bool clock_handle_irq(const struct exception_frame* frame);
