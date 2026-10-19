#pragma once

/* Halt-and-catch-fire path used after unrecoverable kernel failures. This function never returns. */
__attribute__((noreturn))
void hcf(void);

/* Halt without touching diagnostics; safe after a crash report has started or recursively failed. */
__attribute__((noreturn))
void hcf_silent(void);
