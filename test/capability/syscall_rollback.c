#include <base/cap.h>
#include <base/channel.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <core/channel.h>
#include <core/cpu.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <stdint.h>

#include "syscall_test_support.h"

static uintptr_t invalid_cap_output_pointer(void) {
	return UINTPTR_MAX - 3u;
}

static struct process* make_current_process(const char* name) {
	struct process* process;
	struct uthread* main_thread;

	syscall_test_init_process_environment();
	capability_init();
	process = syscall_test_spawn_process(name);
	cr_assert_not_null(process);
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);
	sched_set_current(cpu_current(), &main_thread->thread);
	return process;
}

static void destroy_current_process(struct process* process) {
	struct uthread* main_thread;

	if (process == NULL) return;
	main_thread = process_main_thread(process);
	if (main_thread != NULL) thread_mark_zombie(&main_thread->thread);
	sched_set_current(cpu_current(), NULL);
	cr_assert(process_destroy(process), "failed to destroy capability syscall test process");
	syscall_test_reset_state();
}

Test(capability_syscall, publish_output_failure_rolls_back_new_capability_and_object) {
	struct process*  process   = make_current_process("cap/publish-rollback");
	struct channel*  channel   = channel_create(process_pid(process));
	const uint64_t   object_id = 0x2001u;
	size_t           objects_before;
	size_t           caps_before;
	syscall_result_t result;

	cr_assert_not_null(channel);
	objects_before = capability_object_count();
	caps_before    = capability_count();
	result         = syscall_dispatch(SYSCALL_CAP_CREATE,
                              (uintptr_t)channel->id,
                              (uintptr_t)process_pid(process),
                              (uintptr_t)object_id,
                              (uintptr_t)CAP_READ,
                              invalid_cap_output_pointer(),
                              0u);

	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed publish left an unreachable capability record");
	cr_assert_eq(
		capability_object_count(), objects_before, "failed publish left a newly-created routing object behind");
	cr_assert_null(cap_object_lookup(channel, object_id));

	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}

Test(capability_syscall, publish_output_failure_preserves_preexisting_object) {
	struct process*    process   = make_current_process("cap/publish-existing");
	struct channel*    channel   = channel_create(process_pid(process));
	const uint64_t     object_id = 0x2002u;
	struct cap_object* object;
	size_t             objects_before;
	size_t             caps_before;
	syscall_result_t   result;

	cr_assert_not_null(channel);
	object = cap_object_create(object_id, channel);
	cr_assert_not_null(object);
	objects_before = capability_object_count();
	caps_before    = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_CREATE,
	                          (uintptr_t)channel->id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)object_id,
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed publish leaked a capability for a preexisting object");
	cr_assert_eq(capability_object_count(), objects_before, "rollback must preserve preexisting objects");
	cr_assert_eq(cap_object_lookup(channel, object_id), object);

	cr_assert(cap_object_destroy(object));
	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}

Test(capability_syscall, delegate_output_failure_rolls_back_child_capability) {
	struct process*    process = make_current_process("cap/delegate-rollback");
	struct cap_object* object  = cap_object_create(0x2003u, NULL);
	struct capability* source;
	size_t             caps_before;
	syscall_result_t   result;

	cr_assert_not_null(object);
	source = cap_create(object->cap_object_id, process_pid(process), CAP_READ | CAP_DELEGATE, NULL);
	cr_assert_not_null(source);
	caps_before = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_DELEGATE,
	                          (uintptr_t)source->cap_id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u,
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed delegation left an unreachable child capability");

	cr_assert(cap_destroy(source));
	cr_assert(cap_object_destroy(object));
	destroy_current_process(process);
}

Test(capability_syscall, derive_output_failure_rolls_back_child_and_new_object) {
	struct process*    process = make_current_process("cap/derive-rollback");
	struct channel*    channel = channel_create(process_pid(process));
	struct cap_object* base_object;
	struct capability* base_cap;
	const uint64_t     derived_object_id = 0x2005u;
	size_t             objects_before;
	size_t             caps_before;
	syscall_result_t   result;

	cr_assert_not_null(channel);
	base_object = cap_object_create(0x2004u, channel);
	cr_assert_not_null(base_object);
	base_cap = cap_create(base_object->cap_object_id, process_pid(process), CAP_DERIVE, NULL);
	cr_assert_not_null(base_cap);
	objects_before = capability_object_count();
	caps_before    = capability_count();

	result = syscall_dispatch(SYSCALL_CAP_DERIVE,
	                          (uintptr_t)base_cap->cap_id,
	                          (uintptr_t)process_pid(process),
	                          (uintptr_t)derived_object_id,
	                          (uintptr_t)CAP_READ,
	                          invalid_cap_output_pointer(),
	                          0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(capability_count(), caps_before, "failed derive left an unreachable child capability");
	cr_assert_eq(
		capability_object_count(), objects_before, "failed derive left its newly-created derived object behind");
	cr_assert_null(cap_object_lookup(channel, derived_object_id));

	cr_assert(cap_destroy(base_cap));
	cr_assert(cap_object_destroy(base_object));
	cr_assert_eq(channel_destroy(channel, process_pid(process)), CHANNEL_OK);
	destroy_current_process(process);
}
