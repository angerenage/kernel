#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Remap and mask the legacy programmable interrupt controllers. */
void pic_init(void);

/* Mask one legacy PIC interrupt request line. */
void pic_mask_irq(unsigned irq);

/* Unmask one legacy PIC interrupt request line. */
void pic_unmask_irq(unsigned irq);

/* Return whether a legacy PIC vector is a spurious IRQ7 or IRQ15. */
bool pic_is_spurious_irq(unsigned vector);

/* Signal end-of-interrupt for a legacy PIC vector. */
void pic_send_eoi(unsigned vector);

/* Program the legacy interval timer and return its actual frequency. */
bool pit_init(uint32_t frequency_hz, uint32_t* actual_frequency_hz);
