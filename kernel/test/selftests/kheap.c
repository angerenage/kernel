#include <core/kheap.h>
#include <kernel/selftest.h>
#include <stddef.h>
#include <stdint.h>

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

static const struct kernel_selftest_case kernel_kheap_selftests[] = {
	{
     .name = "allocates_and_restores_heap",
     .run  = kernel_selftest_kheap_allocates_and_restores,
	 },
};

const struct kernel_selftest_suite kernel_kheap_selftest_suite = {
	.name       = "kheap",
	.cases      = kernel_kheap_selftests,
	.case_count = sizeof(kernel_kheap_selftests) / sizeof(kernel_kheap_selftests[0]),
};
