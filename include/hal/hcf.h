#pragma once

/* Halt-and-catch-fire path used after unrecoverable kernel failures. This function never returns. */
__attribute__((noreturn))
void hcf(void);
