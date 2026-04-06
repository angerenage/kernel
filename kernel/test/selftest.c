#include <kernel/requests.h>
#include <kernel/selftest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern const struct kernel_selftest_suite kernel_pmm_selftest_suite;
extern const struct kernel_selftest_suite kernel_vmm_selftest_suite;
extern const struct kernel_selftest_suite kernel_kheap_selftest_suite;
extern const struct kernel_selftest_suite kernel_cpu_selftest_suite;

static const struct kernel_selftest_suite* const kernel_selftest_suites[] = {
	&kernel_cpu_selftest_suite,
	&kernel_pmm_selftest_suite,
	&kernel_vmm_selftest_suite,
	&kernel_kheap_selftest_suite,
};

static bool selftest_is_space(char ch) {
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static bool selftest_token_equals(const char* token, size_t token_len, const char* expected) {
	size_t expected_len = strlen(expected);

	return token_len == expected_len && memcmp(token, expected, token_len) == 0;
}

static bool selftest_token_is_truthy_option(const char* token, size_t token_len, const char* name) {
	size_t name_len;

	if (!token || !name) return false;

	name_len = strlen(name);
	if (token_len == name_len && memcmp(token, name, name_len) == 0) return true;
	if (token_len <= name_len + 1u || memcmp(token, name, name_len) != 0 || token[name_len] != '=') return false;

	token += name_len + 1u;
	token_len -= name_len + 1u;

	return selftest_token_equals(token, token_len, "1") || selftest_token_equals(token, token_len, "true") ||
	       selftest_token_equals(token, token_len, "yes") || selftest_token_equals(token, token_len, "on");
}

void kernel_selftest_fail(struct kernel_selftest_context* ctx, const char* file, size_t line, const char* expr,
                          const char* message) {
	if (!ctx || ctx->failure_expr != NULL) return;

	ctx->failure_expr    = expr;
	ctx->failure_message = message;
	ctx->failure_file    = file;
	ctx->failure_line    = line;
}

bool kernel_selftests_requested(void) {
	const char* cmdline;
	const char* cursor;

	if (!cmdline_req.response || !cmdline_req.response->cmdline) return false;

	cmdline = cmdline_req.response->cmdline;
	cursor  = cmdline;

	while (*cursor != '\0') {
		const char* token_start;
		size_t      token_len;

		while (selftest_is_space(*cursor)) cursor++;
		if (*cursor == '\0') break;

		token_start = cursor;
		while (*cursor != '\0' && !selftest_is_space(*cursor)) cursor++;

		token_len = (size_t)(cursor - token_start);
		if (selftest_token_is_truthy_option(token_start, token_len, "selftest") ||
		    selftest_token_is_truthy_option(token_start, token_len, "selftests") ||
		    selftest_token_is_truthy_option(token_start, token_len, "kernel.selftest") ||
		    selftest_token_is_truthy_option(token_start, token_len, "kernel.selftests")) {
			return true;
		}
	}

	return false;
}

static bool kernel_selftests_run_case(const struct kernel_selftest_suite* suite,
                                      const struct kernel_selftest_case* test_case, size_t* assertion_total) {
	struct kernel_selftest_context ctx = {0};

	test_case->run(&ctx);
	*assertion_total += ctx.assertions;

	if (ctx.failure_expr != NULL) {
		printf("kernel: selftest %s.%s FAIL at %s:%zu (%s)\n",
		       suite->name,
		       test_case->name,
		       ctx.failure_file,
		       ctx.failure_line,
		       ctx.failure_expr);
		if (ctx.failure_message != NULL) printf("kernel: selftest detail: %s\n", ctx.failure_message);
		return false;
	}

	printf("kernel: selftest %s.%s PASS (%zu assertions)\n", suite->name, test_case->name, ctx.assertions);
	return true;
}

bool kernel_selftests_run(void) {
	size_t suite_count     = sizeof(kernel_selftest_suites) / sizeof(kernel_selftest_suites[0]);
	size_t case_total      = 0;
	size_t case_failed     = 0;
	size_t assertion_total = 0;

	printf("kernel: selftests requested, running %zu suite(s)\n", suite_count);

	for (size_t i = 0; i < suite_count; i++) {
		const struct kernel_selftest_suite* suite = kernel_selftest_suites[i];

		for (size_t j = 0; j < suite->case_count; j++) {
			case_total++;
			if (!kernel_selftests_run_case(suite, &suite->cases[j], &assertion_total)) case_failed++;
		}
	}

	printf("kernel: selftest summary: %zu passed, %zu failed, %zu assertions\n",
	       case_total - case_failed,
	       case_failed,
	       assertion_total);
	printf("kernel: selftests result: %s\n", case_failed == 0 ? "PASS" : "FAIL");
	return case_failed == 0;
}
