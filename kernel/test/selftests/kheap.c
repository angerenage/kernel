#include <core/kheap.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../selftest.h"

static void kernel_selftest_kheap_allocates_and_restores(struct kernel_selftest_context* ctx) {
	size_t free_before;
	size_t total_bytes;
	size_t free_during;
	void*  first        = NULL;
	void*  second       = NULL;
	void*  reused       = NULL;
	void*  first_before = NULL;

	free_before = kheap_free_bytes();
	total_bytes = kheap_total_bytes();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_bytes > 0u, "heap reported no capacity");
	KERNEL_SELFTEST_ASSERT(ctx, free_before <= total_bytes);

	first        = kmalloc(24);
	first_before = first;
	second       = kmalloc(80);

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, first != NULL, "kmalloc(24) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, second != NULL, "kmalloc(80) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (((uintptr_t)first) & 0x0fu) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, (((uintptr_t)second) & 0x0fu) == 0u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, first != second, cleanup);

	free_during = kheap_free_bytes();
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, free_during < free_before, "heap free space did not decrease", cleanup);

	kfree(first);
	first = NULL;

	reused = kmalloc(24);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, reused != NULL, "second kmalloc(24) returned NULL", cleanup);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, reused == first_before, "allocator did not reuse the freed block", cleanup);

cleanup:
	if (reused != NULL) kfree(reused);
	if (first != NULL) kfree(first);
	if (second != NULL) kfree(second);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, kheap_free_bytes() == free_before);
}

static void kernel_selftest_kheap_calloc_zeroes_and_realloc_preserves_contents(struct kernel_selftest_context* ctx) {
	size_t   free_before;
	uint8_t* data  = NULL;
	uint8_t* grown = NULL;

	free_before = kheap_free_bytes();

	data = (uint8_t*)kcalloc(32, sizeof(uint8_t));
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, data != NULL, "kcalloc returned NULL", cleanup);

	for (size_t i = 0; i < 32; i++) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, data[i] == 0u, cleanup);
		data[i] = (uint8_t)(0x40u + i);
	}

	grown = (uint8_t*)krealloc(data, 256);
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, grown != NULL, "krealloc returned NULL", cleanup);
	data = NULL;

	for (size_t i = 0; i < 32; i++) {
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, grown[i] == (uint8_t)(0x40u + i), cleanup);
	}

cleanup:
	if (grown != NULL) kfree(grown);
	if (data != NULL) kfree(data);

	if (ctx->failure_expr == NULL) KERNEL_SELFTEST_ASSERT(ctx, kheap_free_bytes() == free_before);
}

static void kernel_selftest_kheap_grows_when_initial_arena_is_exhausted(struct kernel_selftest_context* ctx) {
	void*  blocks[128]   = {0};
	size_t free_before   = kheap_free_bytes();
	size_t total_before  = kheap_total_bytes();
	size_t count         = 0;
	size_t total_current = total_before;

	while (count < (sizeof(blocks) / sizeof(blocks[0])) && total_current == total_before) {
		blocks[count] = kmalloc(256);
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx, blocks[count] != NULL, "kmalloc(256) returned NULL before heap growth", cleanup);
		memset(blocks[count], (int)(0x80u + (count & 0x1fu)), 256);
		count++;
		total_current = kheap_total_bytes();
	}

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, total_current > total_before, "heap did not request additional pages", cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, count > 0u, cleanup);

cleanup:
	for (size_t i = 0; i < count; i++) {
		if (blocks[i] != NULL) kfree(blocks[i]);
	}

	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, kheap_total_bytes() >= total_before);
		KERNEL_SELFTEST_ASSERT(ctx, kheap_free_bytes() >= free_before);
		KERNEL_SELFTEST_ASSERT(ctx, kheap_total_bytes() - kheap_free_bytes() == total_before - free_before);
	}
}

static const struct kernel_selftest_case kernel_kheap_selftests[] = {
	{
     .name = "allocates_and_restores_heap",
     .run  = kernel_selftest_kheap_allocates_and_restores,
	 },
	{
     .name = "calloc_zeroes_and_realloc_preserves_contents",
     .run  = kernel_selftest_kheap_calloc_zeroes_and_realloc_preserves_contents,
	 },
	{
     .name = "grows_when_initial_arena_is_exhausted",
     .run  = kernel_selftest_kheap_grows_when_initial_arena_is_exhausted,
	 },
};

const struct kernel_selftest_suite kernel_kheap_selftest_suite = {
	.name       = "kheap",
	.cases      = kernel_kheap_selftests,
	.case_count = sizeof(kernel_kheap_selftests) / sizeof(kernel_kheap_selftests[0]),
};
