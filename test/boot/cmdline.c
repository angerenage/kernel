#include <boot/info.h>
#include <criterion/criterion.h>
#include <kernel/cmdline.h>
#include <stddef.h>
#include <string.h>

static const char* current_command_line;

const char* boot_cmdline_current(void) {
	return current_command_line;
}

static void expect_userspace_arguments(const char* command_line, size_t expected_argc, const char* expected,
                                       size_t expected_size) {
	char   buffer[256];
	size_t argc;
	size_t size;

	current_command_line = command_line;
	cr_assert(kernel_cmdline_userspace_arguments(NULL, 0u, &argc, &size));
	cr_assert_eq(argc, expected_argc);
	cr_assert_eq(size, expected_size);
	cr_assert_leq(size, sizeof(buffer));
	memset(buffer, 0xa5, sizeof(buffer));
	cr_assert(kernel_cmdline_userspace_arguments(buffer, size, &argc, &size));
	cr_assert_eq(argc, expected_argc);
	cr_assert_eq(size, expected_size);
	cr_assert_eq(memcmp(buffer, expected, expected_size), 0);
}

Test(kernel_cmdline, retains_only_options_unrecognized_by_the_kernel) {
	static const char expected[] = "device_manager.source=dt\0debugger=1\0console.color=true\0kernel.unknown=1\0";

	expect_userspace_arguments("debug kernel.debug=1 loglevel=debug "
	                           "device_manager.source=dt selftest kernel.selftest=1 "
	                           "selftests=no kernel.selftests=no selftest.suite=ipc "
	                           "kernel.selftest.suite=core kernel.selftest.iommu=none "
	                           "debugger=1 console.color=true kernel.unknown=1",
	                           4u,
	                           expected,
	                           sizeof(expected) - 1u);
}

Test(kernel_cmdline, preserves_token_order_and_normalizes_whitespace) {
	static const char expected[] = "first\0second=value\0third\0";

	expect_userspace_arguments(" \tfirst\nloglevel=info\rsecond=value  third\t", 3u, expected, sizeof(expected) - 1u);
}

Test(kernel_cmdline, accepts_empty_or_kernel_only_command_lines) {
	size_t argc = 99u;
	size_t size = 99u;

	current_command_line = NULL;
	cr_assert(kernel_cmdline_userspace_arguments(NULL, 0u, &argc, &size));
	cr_assert_eq(argc, 0u);
	cr_assert_eq(size, 0u);

	expect_userspace_arguments("debug loglevel=quiet kernel.selftest=0", 0u, "", 0u);
}

Test(kernel_cmdline, rejects_invalid_output_and_short_buffers) {
	char   buffer[4];
	size_t argc = 99u;
	size_t size = 99u;

	current_command_line = "option=value";
	cr_assert_not(kernel_cmdline_userspace_arguments(buffer, sizeof(buffer), &argc, &size));
	cr_assert_eq(argc, 0u);
	cr_assert_eq(size, 0u);
	cr_assert_not(kernel_cmdline_userspace_arguments(buffer, sizeof(buffer), NULL, &size));
	cr_assert_not(kernel_cmdline_userspace_arguments(buffer, sizeof(buffer), &argc, NULL));
	cr_assert_not(kernel_cmdline_userspace_arguments(NULL, sizeof(buffer), &argc, &size));
}
