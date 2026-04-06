#pragma once

#include <stdbool.h>
#include <stddef.h>

struct kernel_selftest_context {
	size_t      assertions;
	const char* failure_expr;
	const char* failure_message;
	const char* failure_file;
	size_t      failure_line;
};

typedef void (*kernel_selftest_fn)(struct kernel_selftest_context* ctx);

struct kernel_selftest_case {
	const char*        name;
	kernel_selftest_fn run;
};

struct kernel_selftest_suite {
	const char*                        name;
	const struct kernel_selftest_case* cases;
	size_t                             case_count;
};

void kernel_selftest_fail(struct kernel_selftest_context* ctx, const char* file, size_t line, const char* expr,
                          const char* message);

bool kernel_selftests_requested(void);
bool kernel_selftests_run(void);

#define KERNEL_SELFTEST_ASSERT(ctx, expr)                                                                              \
	do {                                                                                                               \
		(ctx)->assertions++;                                                                                           \
		if (!(expr)) {                                                                                                 \
			kernel_selftest_fail((ctx), __FILE__, __LINE__, #expr, NULL);                                              \
			return;                                                                                                    \
		}                                                                                                              \
	} while (0)

#define KERNEL_SELFTEST_ASSERT_MSG(ctx, expr, message)                                                                 \
	do {                                                                                                               \
		(ctx)->assertions++;                                                                                           \
		if (!(expr)) {                                                                                                 \
			kernel_selftest_fail((ctx), __FILE__, __LINE__, #expr, (message));                                         \
			return;                                                                                                    \
		}                                                                                                              \
	} while (0)

#define KERNEL_SELFTEST_ASSERT_GOTO(ctx, expr, label)                                                                  \
	do {                                                                                                               \
		(ctx)->assertions++;                                                                                           \
		if (!(expr)) {                                                                                                 \
			kernel_selftest_fail((ctx), __FILE__, __LINE__, #expr, NULL);                                              \
			goto label;                                                                                                \
		}                                                                                                              \
	} while (0)

#define KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, expr, message, label)                                                     \
	do {                                                                                                               \
		(ctx)->assertions++;                                                                                           \
		if (!(expr)) {                                                                                                 \
			kernel_selftest_fail((ctx), __FILE__, __LINE__, #expr, (message));                                         \
			goto label;                                                                                                \
		}                                                                                                              \
	} while (0)
