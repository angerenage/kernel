#pragma once

#include <stdint.h>

/* Opaque architecture context retained across a non-local jump. */
typedef struct libc_jump_context {
	uintptr_t registers[32];
} jmp_buf[1];

/* Save the current execution context, returning again after longjmp. */
__attribute__((returns_twice))
int libc_setjmp(jmp_buf environment);

#define setjmp(environment) libc_setjmp(environment)

/* Restore a saved execution context and make setjmp return a nonzero value. */
__attribute__((noreturn))
void longjmp(jmp_buf environment, int value);
