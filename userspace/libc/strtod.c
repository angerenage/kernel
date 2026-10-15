#include <libc/ctype.h>
#include <libc/errno.h>
#include <libc/stdlib.h>
#include <stdbool.h>
#include <stddef.h>

static bool ascii_equal(char first, char second) {
	if (first >= 'A' && first <= 'Z') first += 'a' - 'A';
	return first == second;
}

static double scale_decimal(double value, int exponent, bool* out_range_error) {
	double   original  = value;
	bool     negative  = exponent < 0;
	unsigned magnitude = negative ? (unsigned)(-(exponent + 1)) + 1u : (unsigned)exponent;
	double   factor    = 10.0;
	while (magnitude != 0u) {
		if ((magnitude & 1u) != 0u) value = negative ? value / factor : value * factor;
		magnitude >>= 1u;
		if (magnitude != 0u) factor *= factor;
	}
	if (value > __DBL_MAX__ || (value != 0.0 && value < __DBL_MIN__) || (original != 0.0 && value == 0.0))
		*out_range_error = true;
	return value;
}

double strtod(const char* restrict string, char** restrict end) {
	const char* cursor = string;
	const char* exponent_start;
	bool        negative;
	bool        any             = false;
	double      value           = 0.0;
	int         fraction_digits = 0;
	int         exponent        = 0;
	bool        range_error     = false;

	if (end != NULL) *end = (char*)string;
	if (string == NULL) {
		errno = EINVAL;
		return 0.0;
	}
	while (isspace((unsigned char)*cursor)) cursor++;
	negative = *cursor == '-';
	if (*cursor == '-' || *cursor == '+') cursor++;
	if (ascii_equal(cursor[0], 'i') && ascii_equal(cursor[1], 'n') && ascii_equal(cursor[2], 'f')) {
		cursor += 3;
		if (ascii_equal(cursor[0], 'i') && ascii_equal(cursor[1], 'n') && ascii_equal(cursor[2], 'i') &&
		    ascii_equal(cursor[3], 't') && ascii_equal(cursor[4], 'y'))
			cursor += 5;
		if (end != NULL) *end = (char*)cursor;
		double infinity = __builtin_inf();
		return negative ? -infinity : infinity;
	}
	while (isdigit((unsigned char)*cursor)) {
		any   = true;
		value = value * 10.0 + (double)(*cursor++ - '0');
	}
	if (*cursor == '.') {
		cursor++;
		while (isdigit((unsigned char)*cursor)) {
			any   = true;
			value = value * 10.0 + (double)(*cursor++ - '0');
			if (fraction_digits < 10000) fraction_digits++;
		}
	}
	if (!any) return 0.0;
	exponent_start = cursor;
	if (*cursor == 'e' || *cursor == 'E') {
		const char* exponent_cursor   = cursor + 1;
		bool        exponent_negative = *exponent_cursor == '-';
		if (*exponent_cursor == '-' || *exponent_cursor == '+') exponent_cursor++;
		if (isdigit((unsigned char)*exponent_cursor)) {
			cursor = exponent_cursor;
			while (isdigit((unsigned char)*cursor)) {
				if (exponent < 10000) exponent = exponent * 10 + (*cursor - '0');
				cursor++;
			}
			if (exponent_negative) exponent = -exponent;
		}
		else cursor = exponent_start;
	}
	exponent -= fraction_digits;
	value = scale_decimal(value, exponent, &range_error);
	if (range_error) errno = ERANGE;
	if (end != NULL) *end = (char*)cursor;
	return negative ? -value : value;
}
