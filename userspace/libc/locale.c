#include <libc/limits.h>
#include <libc/locale.h>
#include <libc/string.h>
#include <stddef.h>

static char empty[]    = "";
static char decimal[]  = ".";
static char c_locale[] = "C";

static struct lconv current_locale = {
	.decimal_point      = decimal,
	.thousands_sep      = empty,
	.grouping           = empty,
	.int_curr_symbol    = empty,
	.currency_symbol    = empty,
	.mon_decimal_point  = empty,
	.mon_thousands_sep  = empty,
	.mon_grouping       = empty,
	.positive_sign      = empty,
	.negative_sign      = empty,
	.int_frac_digits    = CHAR_MAX,
	.frac_digits        = CHAR_MAX,
	.p_cs_precedes      = CHAR_MAX,
	.p_sep_by_space     = CHAR_MAX,
	.n_cs_precedes      = CHAR_MAX,
	.n_sep_by_space     = CHAR_MAX,
	.p_sign_posn        = CHAR_MAX,
	.n_sign_posn        = CHAR_MAX,
	.int_p_cs_precedes  = CHAR_MAX,
	.int_p_sep_by_space = CHAR_MAX,
	.int_n_cs_precedes  = CHAR_MAX,
	.int_n_sep_by_space = CHAR_MAX,
	.int_p_sign_posn    = CHAR_MAX,
	.int_n_sign_posn    = CHAR_MAX,
};

struct lconv* localeconv(void) {
	return &current_locale;
}

char* setlocale(int category, const char* locale) {
	if (category < LC_ALL || category > LC_TIME) return NULL;
	if (locale == NULL || locale[0] == '\0' || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0)
		return c_locale;
	return NULL;
}
