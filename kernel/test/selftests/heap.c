#include <base/heap.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../selftest.h"

static void kernel_selftest_heap_allocates_and_restores(struct kernel_selftest_context* ctx) {
	size_t free_before;
	size_t total_bytes;
	size_t free_during;
	void*  first        = NULL;
	void*  second       = NULL;
	void*  reused       = NULL;
	void*  first_before = NULL;

	free_before = heap_free_bytes();
	total_bytes = heap_total_bytes();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_bytes > 0u, "heap reported no capacity");
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= total_bytes);

	first        = malloc(24);
	first_before = first;
	second       = malloc(80);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, first != NULL, "malloc(24) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, second != NULL, "malloc(80) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (((uintptr_t)first) & 0x0fu) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (((uintptr_t)second) & 0x0fu) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, first != second, cleanup);

	free_during = heap_free_bytes();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, free_during < free_before, "heap free space did not decrease", cleanup);

	free(first);
	first = NULL;

	reused = malloc(24);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, reused != NULL, "second malloc(24) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, reused == first_before, "allocator did not reuse the freed block", cleanup);

cleanup:
	if (reused != NULL) free(reused);
	if (first != NULL) free(first);
	if (second != NULL) free(second);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, heap_free_bytes() == free_before);
}

static void kernel_selftest_heap_calloc_zeroes_and_realloc_preserves_contents(struct kernel_selftest_context* ctx) {
	size_t   free_before;
	uint8_t* data  = NULL;
	uint8_t* grown = NULL;

	free_before = heap_free_bytes();

	data = (uint8_t*)calloc(32, sizeof(uint8_t));
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, data != NULL, "calloc returned NULL", cleanup);

	for (size_t i = 0; i < 32; i++) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, data[i] == 0u, cleanup);
		data[i] = (uint8_t)(0x40u + i);
	}

	grown = (uint8_t*)realloc(data, 256);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, grown != NULL, "realloc returned NULL", cleanup);
	data = NULL;

	for (size_t i = 0; i < 32; i++) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, grown[i] == (uint8_t)(0x40u + i), cleanup);
	}

cleanup:
	if (grown != NULL) free(grown);
	if (data != NULL) free(data);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, heap_free_bytes() == free_before);
}

static void kernel_selftest_heap_grows_when_initial_arena_is_exhausted(struct kernel_selftest_context* ctx) {
	void*  blocks[128]   = {0};
	size_t free_before   = heap_free_bytes();
	size_t total_before  = heap_total_bytes();
	size_t count         = 0;
	size_t total_current = total_before;

	while (count < (sizeof(blocks) / sizeof(blocks[0])) && total_current == total_before) {
		blocks[count] = malloc(256);
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, blocks[count] != NULL, "malloc(256) returned NULL before heap growth", cleanup);
		memset(blocks[count], (int)(0x80u + (count & 0x1fu)), 256);
		count++;
		total_current = heap_total_bytes();
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, total_current > total_before, "heap did not request additional pages", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, count > 0u, cleanup);

cleanup:
	for (size_t i = 0; i < count; i++) {
		if (blocks[i] != NULL) free(blocks[i]);
	}

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, heap_total_bytes() >= total_before);
		KERNEL_SELFTEST_ASSERT(ctx, heap_free_bytes() >= free_before);
		KERNEL_SELFTEST_ASSERT(ctx, heap_total_bytes() - heap_free_bytes() == total_before - free_before);
	}
}

static void kernel_selftest_heap_realloc_special_cases_restore_state(struct kernel_selftest_context* ctx) {
	size_t   free_before = heap_free_bytes();
	uint8_t* grown       = NULL;
	uint8_t* direct      = NULL;

	direct = (uint8_t*)realloc(NULL, 64u);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, direct != NULL, "realloc(NULL, 64) returned NULL", cleanup);

	for (size_t i = 0; i < 64u; i++) {
		direct[i] = (uint8_t)(i + 1u);
	}

	grown = (uint8_t*)realloc(direct, 128u);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, grown != NULL, "realloc(direct, 128) returned NULL", cleanup);
	direct = NULL;

	for (size_t i = 0; i < 64u; i++) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, grown[i] == (uint8_t)(i + 1u), cleanup);
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, realloc(grown, 0u) == NULL, "realloc(grown, 0) did not return NULL", cleanup);
	grown = NULL;

cleanup:
	if (grown != NULL) free(grown);
	if (direct != NULL) free(direct);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, heap_free_bytes() == free_before);
}

static const struct kernel_selftest_case kernel_heap_selftests[] = {
	{
     .name = "allocates_and_restores_heap",
     .run  = kernel_selftest_heap_allocates_and_restores,
	 },
	{
     .name = "calloc_zeroes_and_realloc_preserves_contents",
     .run  = kernel_selftest_heap_calloc_zeroes_and_realloc_preserves_contents,
	 },
	{
     .name = "grows_when_initial_arena_is_exhausted",
     .run  = kernel_selftest_heap_grows_when_initial_arena_is_exhausted,
	 },
	{
     .name = "realloc_special_cases_restore_state",
     .run  = kernel_selftest_heap_realloc_special_cases_restore_state,
	 },
};

const struct kernel_selftest_suite kernel_heap_selftest_suite = {
	.name       = "heap",
	.cases      = kernel_heap_selftests,
	.case_count = sizeof(kernel_heap_selftests) / sizeof(kernel_heap_selftests[0]),
};
