#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small overflow-safe arithmetic helpers used heavily by allocators and address-space code. */

/* Add two u64 values and report whether the result overflowed. */
bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t* out);

/* Multiply two u64 values and report whether the result overflowed. */
bool mul_overflow_u64(uint64_t a, uint64_t b, uint64_t* out);

/* Add two size_t values and report whether the result overflowed. */
bool add_overflow_size(size_t a, size_t b, size_t* out);

/* Multiply two size_t values and report whether the result overflowed. */
bool mul_overflow_size(size_t a, size_t b, size_t* out);

/* Round a u64 value up to a power-of-two alignment. */
bool align_up_u64(uint64_t value, uint64_t align, uint64_t* out);

/* Round a u64 value down to a power-of-two alignment. */
uint64_t align_down_u64(uint64_t value, uint64_t align);

/* Round a size_t value up to a power-of-two alignment. */
bool align_up_size(size_t value, size_t align, size_t* out);

/* Replace zero with a default alignment while preserving non-zero caller input. */
uint64_t normalize_align_u64(size_t align, uint64_t default_align);
