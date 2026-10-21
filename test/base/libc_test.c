#include <criterion/criterion.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Test(base_libc, classifies_ascii_characters) {
	cr_assert((isalnum)('A'));
	cr_assert((isalpha)('z'));
	cr_assert((isdigit)('7'));
	cr_assert((isspace)('\n'));
	cr_assert((isxdigit)('f'));
	cr_assert_not((isalpha)('4'));
	cr_assert_eq((tolower)('Q'), 'q');
	cr_assert_eq((toupper)('q'), 'Q');
}

Test(base_libc, searches_strings) {
	const char* value = "keyboard layout";
	cr_assert_eq(strchr(value, 'b'), value + 3);
	cr_assert_eq(strrchr(value, 'a'), value + 10);
	cr_assert_eq(strrchr(value, '\0'), value + strlen(value));
	cr_assert_eq(strchr(value, '\0'), value + strlen(value));
	cr_assert_null(strchr(value, 'z'));
	cr_assert_null(strrchr(value, 'z'));
	cr_assert_eq(strstr(value, "board"), value + 3);
	cr_assert_eq(strstr(value, ""), value);
	cr_assert_null(strstr(value, "mouse"));
}

Test(base_libc, copies_and_concatenates_strings) {
	char buffer[16];
	char padded[8]                                                      = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
	char* (*copy_bounded)(char* restrict, const char* restrict, size_t) = strncpy;

	cr_assert_eq(strcpy(buffer, "frame"), buffer);
	cr_assert_eq(strcat(buffer, "buffer"), buffer);
	cr_assert_str_eq(buffer, "framebuffer");
	cr_assert_eq(strncpy(padded, "PS/2", sizeof(padded)), padded);
	cr_assert_arr_eq(padded, ((const char[8]){'P', 'S', '/', '2', 0, 0, 0, 0}), sizeof(padded));
	copy_bounded(padded, "keyboard", 4u);
	cr_assert_arr_eq(padded, ((const char[4]){'k', 'e', 'y', 'b'}), 4u);
}

static int compare_ints(const void* left, const void* right) {
	int lhs = *(const int*)left;
	int rhs = *(const int*)right;
	return (lhs > rhs) - (lhs < rhs);
}

Test(base_libc, sorts_arrays) {
	int values[]   = {4, -2, 4, 0, 9, 1};
	int expected[] = {-2, 0, 1, 4, 4, 9};

	qsort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]), compare_ints);
	cr_assert_arr_eq(values, expected, sizeof(values));
}

Test(base_libc, converts_integers_and_reals) {
	char* end;
	cr_assert_eq(strtol(" -0x2a trailing", &end, 0), -42);
	cr_assert_str_eq(end, " trailing");
	cr_assert_eq(strtol("999999999999999999999999", &end, 10), LONG_MAX);
	cr_assert_eq(strtoull("18446744073709551615!", &end, 10), ULLONG_MAX);
	cr_assert_str_eq(end, "!");
	cr_assert_eq(strtoull("18446744073709551616", &end, 10), ULLONG_MAX);
	cr_assert_eq(strtoull("-1", &end, 10), ULLONG_MAX);
	cr_assert_str_eq(end, "");

	double value = strtod("  -12.5e2!", &end);
	cr_assert_float_eq(value, -1250.0, 0.000001);
	cr_assert_str_eq(end, "!");
}

Test(base_libc, formats_bounded_strings_and_reals) {
	char buffer[32];
	char small[5];
	int (*format_string)(char* restrict, size_t, const char* restrict, ...) = snprintf;
	cr_assert_eq(snprintf(buffer, sizeof(buffer), "%s:%ld:%g", "value", 42l, 123.5), 14);
	cr_assert_str_eq(buffer, "value:42:123.5");
	cr_assert_eq(format_string(small, sizeof(small), "abcdef"), 6);
	cr_assert_str_eq(small, "abcd");
	cr_assert_eq(snprintf(NULL, 0u, "%g", 0.0000125), 8);
}

Test(base_libc, exposes_the_c_locale) {
	cr_assert_str_eq(setlocale(0, NULL), "C");
	cr_assert_str_eq(setlocale(LC_NUMERIC, "POSIX"), "C");
	cr_assert_null(setlocale(0, "fr_FR.UTF-8"));
	cr_assert_str_eq(localeconv()->decimal_point, ".");
}
