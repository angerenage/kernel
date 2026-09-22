#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct fixed_controller;

/* Map and initialize a PCH programmable interrupt controller. */
bool loongarch64_pch_pic_probe(struct fixed_controller* controller);

/* Return whether a PCH-PIC source is reserved for an internal cascade. */
bool loongarch64_pch_pic_source_reserved(const struct fixed_controller* controller, uint32_t source);

/* Change the mask state of one PCH-PIC source and its parent vector. */
bool loongarch64_pch_pic_set_masked(struct fixed_controller* controller, uint32_t source, bool masked);

/* Change a PCH-PIC source mask and its selected upstream vector state. */
bool loongarch64_pch_pic_set_masked_route(struct fixed_controller* controller, uint32_t source, uint32_t route,
                                          bool masked);

/* Configure and route one PCH-PIC source. */
bool loongarch64_pch_pic_configure(struct fixed_controller* controller, uint32_t source, uint32_t route,
                                   const struct hal_interrupt_delivery* delivery);

/* Mask the PCH leaf controller responsible for a delivered vector. */
bool loongarch64_pch_pic_mask_vector_leaf(uint32_t vector);
