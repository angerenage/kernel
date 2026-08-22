#include "../../userspace/programs/loader/lifecycle.h"

#include <criterion/criterion.h>
#include <system/capability.h>

static syscall_status_t conditional_status;
static syscall_status_t terminal_status;
static size_t           conditional_calls;
static size_t           terminal_calls;

syscall_status_t cap_unpublish(channel_id_t endpoint, uint64_t object_id) {
	(void)endpoint;
	(void)object_id;
	terminal_calls++;
	return terminal_status;
}

syscall_status_t cap_unpublish_if_unused(channel_id_t endpoint, uint64_t object_id) {
	(void)endpoint;
	(void)object_id;
	conditional_calls++;
	return conditional_status;
}

static void reset_unpublish_mocks(void) {
	conditional_status = SYSCALL_STATUS_FAILED;
	terminal_status    = SYSCALL_STATUS_FAILED;
	conditional_calls  = 0u;
	terminal_calls     = 0u;
}

Test(loader_lifecycle, abandoned_load_is_discardable_only_after_conditional_unpublish) {
	struct loader_loaded_program program = {.load_cap = 7u};

	reset_unpublish_mocks();
	cr_assert_not(loader_unpublish_abandoned(3u, &program));
	cr_assert_eq(program.load_cap, 7u);
	cr_assert_eq(conditional_calls, 1u);
	cr_assert_eq(terminal_calls, 0u);

	conditional_status = SYSCALL_STATUS_OK;
	cr_assert(loader_unpublish_abandoned(3u, &program));
	cr_assert_eq(program.load_cap, CAP_ID_INVALID);
	cr_assert_eq(conditional_calls, 2u);
	cr_assert_eq(terminal_calls, 0u);
}

Test(loader_lifecycle, terminal_run_and_cancel_cleanup_remains_unconditional) {
	struct loader_loaded_program program = {.load_cap = 8u};

	reset_unpublish_mocks();
	terminal_status = SYSCALL_STATUS_OK;
	cr_assert_eq(loader_unpublish_terminal(4u, &program), SYSCALL_STATUS_OK);
	cr_assert_eq(program.load_cap, CAP_ID_INVALID);
	cr_assert_eq(terminal_calls, 1u);
	cr_assert_eq(conditional_calls, 0u);
}
