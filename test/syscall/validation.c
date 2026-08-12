#include <base/cap.h>
#include <base/module.h>
#include <base/process.h>
#include <core/capability.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <kernel/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../kernel/src/capability/address_space.h"
#include "../../kernel/src/capability/boot_module.h"
#include "../../kernel/src/capability/process.h"
#include "../../kernel/src/capability/thread.h"
#include "../../kernel/src/syscall/module.h"
#include "../../kernel/src/syscall/process.h"
#include "test_support.h"

enum grant_slot {
	GRANT_SELF = 0,
	GRANT_ADDRESS_SPACE,
	GRANT_MAIN_THREAD,
	GRANT_PROCESS,
	GRANT_SLOT_COUNT,
};

static cap_id_t        grant_caps[GRANT_SLOT_COUNT];
static cap_object_id_t grant_objects[GRANT_SLOT_COUNT];
static enum grant_slot grant_fail_slot = GRANT_SLOT_COUNT;

void kernel_boot_mock_set_modules(const struct kernel_boot_module* modules, size_t count);
void kernel_boot_mock_reset(void);

static syscall_result_t grant_test_handler(const struct cap_request* request) {
	(void)request;
	return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
}

static cap_id_t ensure_test_grant(enum grant_slot slot, uint64_t object_id, process_id_t recipient, cap_rights_t rights,
                                  bool* out_created) {
	cap_id_t cap;

	if (out_created != NULL) *out_created = false;
	if (slot >= GRANT_SLOT_COUNT || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	if (slot == grant_fail_slot) return CAP_ID_INVALID;

	if (grant_caps[slot] != CAP_ID_INVALID) {
		struct capability* retained = cap_acquire(grant_caps[slot]);
		if (retained != NULL) {
			cap_release(retained);
			return grant_caps[slot];
		}
		grant_caps[slot] = CAP_ID_INVALID;
	}

	grant_objects[slot] = cap_object_create_kernel(object_id, grant_test_handler, NULL);
	if (grant_objects[slot] == CAP_OBJECT_ID_INVALID) return CAP_ID_INVALID;

	cap = cap_create(grant_objects[slot], recipient, rights, NULL, out_created);
	if (cap == CAP_ID_INVALID) return CAP_ID_INVALID;
	grant_caps[slot] = cap;
	return cap;
}

static void grant_test_reset(void) {
	for (size_t i = 0u; i < GRANT_SLOT_COUNT; i++) {
		grant_caps[i]    = CAP_ID_INVALID;
		grant_objects[i] = CAP_OBJECT_ID_INVALID;
	}
	grant_fail_slot = GRANT_SLOT_COUNT;
}

static void grant_test_cleanup(void) {
	for (size_t i = 0u; i < GRANT_SLOT_COUNT; i++) {
		if (grant_caps[i] != CAP_ID_INVALID) {
			(void)cap_destroy_by_id(grant_caps[i]);
			grant_caps[i] = CAP_ID_INVALID;
		}
	}
	for (size_t i = 0u; i < GRANT_SLOT_COUNT; i++) {
		if (grant_objects[i] != CAP_OBJECT_ID_INVALID) {
			(void)cap_object_destroy_with_id(grant_objects[i]);
			grant_objects[i] = CAP_OBJECT_ID_INVALID;
		}
	}
}

cap_id_t kernel_process_grant(struct process* target, process_id_t recipient, cap_rights_t rights, bool* out_created) {
	if (target == NULL) return CAP_ID_INVALID;
	cap_id_t cap = ensure_test_grant(GRANT_PROCESS, 0xf100u, recipient, rights, out_created);
	if (cap != CAP_ID_INVALID) process_set_cap_object_id(target, grant_objects[GRANT_PROCESS]);
	return cap;
}

cap_id_t kernel_self_grant(struct process* process, bool* out_created) {
	if (process == NULL) return CAP_ID_INVALID;
	cap_id_t cap =
		ensure_test_grant(GRANT_SELF,
	                      0xf101u,
	                      process_pid(process),
	                      CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_EXEC | CAP_DELEGATE,
	                      out_created);
	if (cap != CAP_ID_INVALID) process_set_cap_object_id(process, grant_objects[GRANT_SELF]);
	return cap;
}

cap_id_t kernel_address_space_grant(struct process* process, process_id_t recipient, cap_rights_t rights,
                                    bool* out_created) {
	if (process == NULL) return CAP_ID_INVALID;
	cap_id_t cap = ensure_test_grant(GRANT_ADDRESS_SPACE, 0xf102u, recipient, rights, out_created);
	if (cap != CAP_ID_INVALID) process_set_address_space_cap_object_id(process, grant_objects[GRANT_ADDRESS_SPACE]);
	return cap;
}

cap_id_t kernel_thread_grant_full(struct uthread* target, process_id_t recipient, bool* out_created) {
	if (target == NULL) return CAP_ID_INVALID;
	cap_id_t cap = ensure_test_grant(GRANT_MAIN_THREAD,
	                                 0xf103u,
	                                 recipient,
	                                 CAP_CALL | CAP_READ | CAP_WAIT | CAP_MANAGE | CAP_DESTROY | CAP_DELEGATE,
	                                 out_created);
	if (cap != CAP_ID_INVALID) uthread_set_cap_object_id(target, grant_objects[GRANT_MAIN_THREAD]);
	return cap;
}

cap_id_t kernel_capability_boot_module_grant(size_t module_index, process_id_t recipient, bool* out_created) {
	(void)module_index;
	(void)recipient;
	if (out_created != NULL) *out_created = false;
	return CAP_ID_INVALID;
}

static struct process* make_current_process(const char* name) {
	struct process* process;
	struct uthread* main_thread;

	syscall_test_init_process_environment();
	capability_init();
	grant_test_reset();

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
	grant_test_cleanup();
	cr_assert(process_destroy(process), "failed to destroy syscall validation process");
	kernel_boot_mock_reset();
	syscall_test_reset_state();
}

Test(syscall_validation, module_resolve_rejects_null_mandatory_name) {
	const struct kernel_boot_module modules[] = {
		{
         .name       = "init",
         .path       = "/boot/init",
         .address    = (void*)(uintptr_t)0x1000u,
         .size       = 4096u,
         .media_type = 0u,
		 },
	};
	struct process*  process;
	syscall_result_t result;

	process = make_current_process("syscall/module-null");
	kernel_boot_mock_set_modules(modules, 1u);

	result = syscall_module_resolve(0u, 0u, 1u, 0u, 0u, 0u);
	cr_assert_eq(result.status,
	             SYSCALL_STATUS_BAD_ARGUMENT,
	             "a mandatory module name cannot use the generic optional-string NULL/zero form");
	cr_assert_eq(result.value, 0u, "the invalid pointer is argument 0");

	destroy_current_process(process);
}

Test(syscall_validation, failed_self_copyout_does_not_publish_hidden_grants) {
	struct process*  process;
	syscall_result_t result;
	size_t           caps_before;
	size_t           objects_before;

	process        = make_current_process("syscall/self-rollback");
	caps_before    = capability_count();
	objects_before = capability_object_count();

	result = syscall_self(MM_USER_VMM_BASE, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT, "unmapped self-info output must reject the syscall");
	cr_assert_eq(capability_count(),
	             caps_before,
	             "failed self copyout must not leave capabilities that userspace never received");
	cr_assert_eq(capability_object_count(),
	             objects_before,
	             "failed self copyout must not publish routing objects solely for the failed query");

	destroy_current_process(process);
}

Test(syscall_validation, failed_self_copyout_preserves_preexisting_grants) {
	struct process*  process;
	struct uthread*  main_thread;
	cap_id_t         self_cap;
	cap_id_t         address_cap;
	cap_id_t         thread_cap;
	syscall_result_t result;
	size_t           caps_before;
	size_t           objects_before;

	process     = make_current_process("syscall/self-existing");
	main_thread = process_main_thread(process);
	cr_assert_not_null(main_thread);

	self_cap = kernel_self_grant(process, NULL);
	address_cap =
		kernel_address_space_grant(process, process_pid(process), CAP_CALL | CAP_MAP | CAP_READ | CAP_DELEGATE, NULL);
	thread_cap = kernel_thread_grant_full(main_thread, process_pid(process), NULL);
	cr_assert_neq(self_cap, CAP_ID_INVALID);
	cr_assert_neq(address_cap, CAP_ID_INVALID);
	cr_assert_neq(thread_cap, CAP_ID_INVALID);

	caps_before    = capability_count();
	objects_before = capability_object_count();

	result = syscall_self(MM_USER_VMM_BASE, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(
		capability_count(), caps_before, "copyout rollback must not destroy grants that existed before the syscall");
	cr_assert_eq(
		capability_object_count(), objects_before, "copyout rollback must preserve preexisting routing objects");

	{
		struct capability* cap = cap_acquire(self_cap);
		cr_assert_not_null(cap);
		cap_release(cap);
	}
	{
		struct capability* cap = cap_acquire(address_cap);
		cr_assert_not_null(cap);
		cap_release(cap);
	}
	{
		struct capability* cap = cap_acquire(thread_cap);
		cr_assert_not_null(cap);
		cap_release(cap);
	}

	destroy_current_process(process);
}

static uintptr_t allocate_self_info_output(struct process* process, vmm_id_t* out_id) {
	void* base = NULL;

	*out_id = VMM_ID_INVALID;
	cr_assert(vmm_alloc(process_address_space(process),
	                    &(const struct vmm_alloc_params){
							.page_count  = 1u,
							.align_pages = 1u,
							.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
							.kind        = VMM_KIND_GENERIC,
						},
	                    out_id,
	                    &base));
	cr_assert_not_null(base);
	return (uintptr_t)base;
}

Test(syscall_validation, failed_address_space_grant_rolls_back_new_self_grant) {
	struct process*  process;
	vmm_id_t         output_id = VMM_ID_INVALID;
	uintptr_t        output;
	size_t           caps_before;
	size_t           objects_before;
	syscall_result_t result;

	process         = make_current_process("syscall/self-address-grant-failure");
	output          = allocate_self_info_output(process, &output_id);
	caps_before     = capability_count();
	objects_before  = capability_object_count();
	grant_fail_slot = GRANT_ADDRESS_SPACE;

	result = syscall_self(output, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(capability_count(),
	             caps_before,
	             "failed address-space grant left the earlier self capability hidden from userspace");
	cr_assert_eq(capability_object_count(),
	             objects_before,
	             "failed address-space grant left the earlier self routing object hidden from userspace");

	grant_fail_slot = GRANT_SLOT_COUNT;
	cr_assert(vmm_free(process_address_space(process), output_id));
	destroy_current_process(process);
}

Test(syscall_validation, failed_thread_grant_rolls_back_all_new_preceding_grants) {
	struct process*  process;
	vmm_id_t         output_id = VMM_ID_INVALID;
	uintptr_t        output;
	size_t           caps_before;
	size_t           objects_before;
	syscall_result_t result;

	process         = make_current_process("syscall/self-thread-grant-failure");
	output          = allocate_self_info_output(process, &output_id);
	caps_before     = capability_count();
	objects_before  = capability_object_count();
	grant_fail_slot = GRANT_MAIN_THREAD;

	result = syscall_self(output, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(capability_count(),
	             caps_before,
	             "failed thread grant left earlier self/address-space capabilities hidden from userspace");
	cr_assert_eq(capability_object_count(),
	             objects_before,
	             "failed thread grant left earlier routing objects hidden from userspace");

	grant_fail_slot = GRANT_SLOT_COUNT;
	cr_assert(vmm_free(process_address_space(process), output_id));
	destroy_current_process(process);
}

Test(syscall_validation, grant_failure_rollback_preserves_preexisting_self_grant) {
	struct process*    process;
	vmm_id_t           output_id = VMM_ID_INVALID;
	uintptr_t          output;
	cap_id_t           existing_self;
	struct capability* retained;
	size_t             caps_before;
	size_t             objects_before;
	syscall_result_t   result;

	process       = make_current_process("syscall/self-existing-grant-failure");
	output        = allocate_self_info_output(process, &output_id);
	existing_self = kernel_self_grant(process, NULL);
	cr_assert_neq(existing_self, CAP_ID_INVALID);
	caps_before     = capability_count();
	objects_before  = capability_object_count();
	grant_fail_slot = GRANT_ADDRESS_SPACE;

	result = syscall_self(output, 0u, 0u, 0u, 0u, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_FAILED);
	cr_assert_eq(capability_count(), caps_before, "rollback changed the preexisting grant set");
	cr_assert_eq(capability_object_count(), objects_before, "rollback changed the preexisting routing object set");
	retained = cap_acquire(existing_self);
	cr_assert_not_null(retained, "rollback destroyed a self capability that existed before the failed syscall");
	cap_release(retained);

	grant_fail_slot = GRANT_SLOT_COUNT;
	cr_assert(vmm_free(process_address_space(process), output_id));
	destroy_current_process(process);
}
