#include <libc/ctype.h>
#include <libc/errno.h>
#include <libc/limits.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>

static int digit_value(unsigned char character) {
	if (character >= '0' && character <= '9') return character - '0';
	if (character >= 'a' && character <= 'z') return character - 'a' + 10;
	if (character >= 'A' && character <= 'Z') return character - 'A' + 10;
	return -1;
}

long strtol(const char* restrict string, char** restrict end, int base) {
	const char*   cursor = string;
	bool          negative;
	bool          any   = false;
	unsigned long value = 0u;
	unsigned long limit;
	bool          overflow = false;

	if (end != NULL) *end = (char*)string;
	if (string == NULL || base < 0 || base == 1 || base > 36) {
		errno = EINVAL;
		return 0;
	}
	while (isspace((unsigned char)*cursor)) cursor++;
	negative = *cursor == '-';
	if (*cursor == '-' || *cursor == '+') cursor++;
	if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') &&
	    digit_value((unsigned char)cursor[2]) >= 0 && digit_value((unsigned char)cursor[2]) < 16) {
		base = 16;
		cursor += 2;
	}
	else if (base == 0) {
		base = cursor[0] == '0' ? 8 : 10;
	}
	limit = negative ? (unsigned long)LONG_MAX + 1u : (unsigned long)LONG_MAX;
	for (;;) {
		int digit = digit_value((unsigned char)*cursor);
		if (digit < 0 || digit >= base) break;
		any = true;
		if (value > (limit - (unsigned long)digit) / (unsigned long)base) overflow = true;
		else value = value * (unsigned long)base + (unsigned long)digit;
		cursor++;
	}
	if (!any) return 0;
	if (end != NULL) *end = (char*)cursor;
	if (overflow) {
		errno = ERANGE;
		return negative ? LONG_MIN : LONG_MAX;
	}
	if (negative && value == (unsigned long)LONG_MAX + 1u) return LONG_MIN;
	return negative ? -(long)value : (long)value;
}

unsigned long long strtoull(const char* restrict string, char** restrict end, int base) {
	const char*        cursor = string;
	bool               negative;
	bool               any      = false;
	unsigned long long value    = 0u;
	bool               overflow = false;

	if (end != NULL) *end = (char*)string;
	if (string == NULL || base < 0 || base == 1 || base > 36) {
		errno = EINVAL;
		return 0u;
	}
	while (isspace((unsigned char)*cursor)) cursor++;
	negative = *cursor == '-';
	if (*cursor == '-' || *cursor == '+') cursor++;
	if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X') &&
	    digit_value((unsigned char)cursor[2]) >= 0 && digit_value((unsigned char)cursor[2]) < 16) {
		base = 16;
		cursor += 2;
	}
	else if (base == 0) {
		base = cursor[0] == '0' ? 8 : 10;
	}
	for (;;) {
		int digit = digit_value((unsigned char)*cursor);
		if (digit < 0 || digit >= base) break;
		any = true;
		if (value > (ULLONG_MAX - (unsigned long long)digit) / (unsigned long long)base) overflow = true;
		else value = value * (unsigned long long)base + (unsigned long long)digit;
		cursor++;
	}
	if (!any) return 0u;
	if (end != NULL) *end = (char*)cursor;
	if (overflow) {
		errno = ERANGE;
		return ULLONG_MAX;
	}
	return negative ? 0u - value : value;
}
