#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small overflow-safe arithmetic helpers used heavily by allocators and address-space code. */

/* Add two u64 values and report whether the result overflowed. */
static inline bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
	return __builtin_add_overflow(a, b, out);
#endif
#endif
	*out = a + b;
	return *out < a;
}

/* Multiply two u64 values and report whether the result overflowed. */
static inline bool mul_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
	return __builtin_mul_overflow(a, b, out);
#endif
#endif
	if (a && b > UINT64_MAX / a) return true;
	*out = a * b;
	return false;
}

/* Add two size_t values and report whether the result overflowed. */
static inline bool add_overflow_size(size_t a, size_t b, size_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_add_overflow)
	return __builtin_add_overflow(a, b, out);
#endif
#endif
	*out = a + b;
	return *out < a;
}

/* Multiply two size_t values and report whether the result overflowed. */
static inline bool mul_overflow_size(size_t a, size_t b, size_t* out) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_mul_overflow)
	return __builtin_mul_overflow(a, b, out);
#endif
#endif
	if (a && b > (size_t)-1 / a) return true;
	*out = a * b;
	return false;
}

/* Round a u64 value up to a power-of-two alignment. */
static inline bool align_up_u64(uint64_t value, uint64_t align, uint64_t* out) {
	uint64_t tmp;

	if (!align || (align & (align - 1u)) != 0) return false;
	if (add_overflow_u64(value, align - 1u, &tmp)) return false;

	*out = tmp & ~(align - 1u);
	return true;
}

/* Round a u64 value down to a power-of-two alignment. */
static inline uint64_t align_down_u64(uint64_t value, uint64_t align) {
	return value & ~(align - 1u);
}

/* Round a size_t value up to a power-of-two alignment. */
static inline bool align_up_size(size_t value, size_t align, size_t* out) {
	size_t tmp;

	if (!align || (align & (align - 1u)) != 0) return false;
	if (add_overflow_size(value, align - 1u, &tmp)) return false;

	*out = tmp & ~(align - 1u);
	return true;
}

/* Replace zero with a default alignment while preserving non-zero caller input. */
static inline uint64_t normalize_align_u64(size_t align, uint64_t default_align) {
	uint64_t normalized = (uint64_t)align;
	if (normalized == 0) normalized = default_align;
	return normalized;
}
