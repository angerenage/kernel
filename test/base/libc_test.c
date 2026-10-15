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
	cr_assert_eq(strchr(value, '\0'), value + strlen(value));
	cr_assert_null(strchr(value, 'z'));
	cr_assert_eq(strstr(value, "board"), value + 3);
	cr_assert_eq(strstr(value, ""), value);
	cr_assert_null(strstr(value, "mouse"));
}

Test(base_libc, converts_integers_and_reals) {
	char* end;
	cr_assert_eq(strtol(" -0x2a trailing", &end, 0), -42);
	cr_assert_str_eq(end, " trailing");
	cr_assert_eq(strtol("999999999999999999999999", &end, 10), LONG_MAX);

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
