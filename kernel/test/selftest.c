#include "selftest.h"

#include <kernel/cmdline.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

extern const struct kernel_selftest_suite kernel_pmm_selftest_suite;
extern const struct kernel_selftest_suite kernel_vmm_selftest_suite;
extern const struct kernel_selftest_suite kernel_heap_selftest_suite;
extern const struct kernel_selftest_suite kernel_thread_bootstrap_selftest_suite;
extern const struct kernel_selftest_suite kernel_kthread_selftest_suite;
extern const struct kernel_selftest_suite kernel_cpu_selftest_suite;
extern const struct kernel_selftest_suite kernel_sched_mutex_selftest_suite;
extern const struct kernel_selftest_suite kernel_semaphore_selftest_suite;
extern const struct kernel_selftest_suite kernel_condvar_selftest_suite;
extern const struct kernel_selftest_suite kernel_rwlock_selftest_suite;

static const struct kernel_selftest_suite* const kernel_selftest_suites[] = {
	&kernel_cpu_selftest_suite,
	&kernel_thread_bootstrap_selftest_suite,
	&kernel_pmm_selftest_suite,
	&kernel_vmm_selftest_suite,
	&kernel_heap_selftest_suite,
	&kernel_kthread_selftest_suite,
	&kernel_sched_mutex_selftest_suite,
	&kernel_semaphore_selftest_suite,
	&kernel_condvar_selftest_suite,
	&kernel_rwlock_selftest_suite,
};

void kernel_selftest_fail(struct kernel_selftest_context* ctx, const char* file, size_t line, const char* expr,
                          const char* message) {
	if (!ctx || ctx->failure_expr != NULL) return;

	ctx->failure_expr    = expr;
	ctx->failure_message = message;
	ctx->failure_file    = file;
	ctx->failure_line    = line;
}

bool kernel_selftests_requested(void) {
	return kernel_cmdline_option_enabled("selftest") || kernel_cmdline_option_enabled("selftests") ||
	       kernel_cmdline_option_enabled("kernel.selftest") || kernel_cmdline_option_enabled("kernel.selftests");
}

static bool selftest_cmdline_requested_suite(const char** suite_name, size_t* suite_name_len) {
	if (!suite_name || !suite_name_len) return false;

	if (kernel_cmdline_option_value("kernel.selftest.suite", suite_name, suite_name_len)) {
		return true;
	}
	return kernel_cmdline_option_value("selftest.suite", suite_name, suite_name_len);
}

static bool kernel_selftests_run_case(const struct kernel_selftest_suite* suite,
                                      const struct kernel_selftest_case* test_case, size_t* assertion_total) {
	struct kernel_selftest_context ctx = {0};

	test_case->run(&ctx);
	*assertion_total += ctx.assertions;

	if (ctx.failure_expr != NULL) {
		printf("selftest: %s.%s FAIL at %s:%zu (%s)\n",
		       suite->name,
		       test_case->name,
		       ctx.failure_file,
		       ctx.failure_line,
		       ctx.failure_expr);
		if (ctx.failure_message != NULL) printf("selftest: detail: %s\n", ctx.failure_message);
		return false;
	}

	printf("selftest: %s.%s PASS (%zu assertions)\n", suite->name, test_case->name, ctx.assertions);
	return true;
}

bool kernel_selftests_run(void) {
	size_t      suite_count     = sizeof(kernel_selftest_suites) / sizeof(kernel_selftest_suites[0]);
	size_t      case_total      = 0;
	size_t      case_failed     = 0;
	size_t      assertion_total = 0;
	const char* suite_filter;
	size_t      suite_filter_len;
	bool        has_suite_filter;
	bool        ran_any_suite = false;

	has_suite_filter = selftest_cmdline_requested_suite(&suite_filter, &suite_filter_len);

	if (has_suite_filter) {
		printf("selftest: requested, running suite '%.*s'\n", (int)suite_filter_len, suite_filter);
	}
	else {
		printf("selftest: requested, running %zu suite(s)\n", suite_count);
	}

	for (size_t i = 0; i < suite_count; i++) {
		const struct kernel_selftest_suite* suite = kernel_selftest_suites[i];

		if (has_suite_filter && !kernel_cmdline_value_equals(suite_filter, suite_filter_len, suite->name)) continue;

		ran_any_suite = true;
		for (size_t j = 0; j < suite->case_count; j++) {
			case_total++;
			if (!kernel_selftests_run_case(suite, &suite->cases[j], &assertion_total)) case_failed++;
		}
	}

	if (!ran_any_suite) {
		printf("selftest: suite '%.*s' not found\n", (int)suite_filter_len, suite_filter);
		printf("selftest: result: FAIL\n");
		return false;
	}

	printf("selftest: summary: %zu passed, %zu failed, %zu assertions\n",
	       case_total - case_failed,
	       case_failed,
	       assertion_total);
	printf("selftest: result: %s\n", case_failed == 0 ? "PASS" : "FAIL");
	return case_failed == 0;
}
