#include <core/process.h>
#include <pthread.h>

#include "test_support.h"

struct target_destroy_race {
	struct process* process;
	bool            destroyed;
};

static process_id_t blocking_cleanup_target = PROCESS_PID_INVALID;
static bool         blocking_cleanup_entered;
static bool         blocking_cleanup_release;

static syscall_result_t target_lifetime_handler(const struct cap_request* request) {
	(void)request;
	return syscall_result_ok(0u);
}

static bool blocking_process_cleanup(uint64_t object_id, process_id_t process) {
	(void)object_id;
	if (process != blocking_cleanup_target) return false;
	__atomic_store_n(&blocking_cleanup_entered, true, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&blocking_cleanup_release, __ATOMIC_ACQUIRE)) {
	}
	return false;
}

static void* destroy_target_worker(void* argument) {
	struct target_destroy_race* race = argument;
	race->destroyed                  = process_destroy(race->process);
	return NULL;
}

Test(capability, grant_target_requires_a_live_process) {
	struct cap_object* object;
	cap_object_id_t    object_id;

	cap_test_setup();
	object_id = cap_object_create(0x400u, NULL, NULL);
	object    = cap_object_acquire(object_id);
	cr_assert_not_null(object);

	cr_assert_eq(cap_create(object_id, PROCESS_PID_INVALID, CAP_READ, NULL), CAP_ID_INVALID);
	cr_assert_eq(cap_create(object_id, (process_id_t)0x100000u, CAP_READ, NULL), CAP_ID_INVALID);
	cr_assert_eq(cap_object_grant_count(object), 0u);

	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
}

Test(capability, grant_target_accepts_new_process_and_rejects_terminated_process) {
	struct cap_object* object;
	struct process*    target = NULL;
	cap_object_id_t    object_id;
	cap_id_t           grant;
	process_id_t       pid;

	cap_test_setup();
	cr_assert_eq(process_create(&target, NULL), PROCESS_OK);
	cr_assert_not_null(target);
	pid       = process_pid(target);
	object_id = cap_object_create(0x401u, NULL, NULL);
	object    = cap_object_acquire(object_id);
	cr_assert_not_null(object);

	grant = cap_create(object_id, pid, CAP_READ, NULL);
	cr_assert_neq(grant, CAP_ID_INVALID, "NEW processes must accept capability grants");
	cr_assert(cap_destroy_by_id(grant));
	cr_assert_eq(cap_object_grant_count(object), 0u);

	cr_assert(process_terminate(target, PROCESS_EXIT_SYSTEM_UNKNOWN));
	cr_assert_eq(process_get_state(target), PROCESS_STATE_ZOMBIE);
	cr_assert_eq(cap_create(object_id, pid, CAP_READ, NULL), CAP_ID_INVALID);
	cr_assert_eq(cap_object_grant_count(object), 0u);

	cr_assert(process_destroy(target));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
}

Test(capability, grant_target_is_closed_before_destroy_cleanup) {
	struct target_destroy_race race = {0};
	struct cap_object*         grant_object;
	struct process*            target = NULL;
	struct process*            held;
	cap_object_id_t            cleanup_object_id;
	cap_object_id_t            grant_object_id;
	process_id_t               pid;
	pthread_t                  destroy_thread;

	cap_test_setup();
	cr_assert_eq(process_create(&target, NULL), PROCESS_OK);
	cr_assert_not_null(target);
	pid = process_pid(target);

	cleanup_object_id =
		cap_object_create_kernel_managed(0x402u, target_lifetime_handler, blocking_process_cleanup, NULL, NULL);
	cr_assert_neq(cleanup_object_id, CAP_OBJECT_ID_INVALID);
	grant_object_id = cap_object_create(0x403u, NULL, NULL);
	grant_object    = cap_object_acquire(grant_object_id);
	cr_assert_not_null(grant_object);

	blocking_cleanup_target = pid;
	__atomic_store_n(&blocking_cleanup_entered, false, __ATOMIC_RELEASE);
	__atomic_store_n(&blocking_cleanup_release, false, __ATOMIC_RELEASE);
	race.process = target;
	cr_assert_eq(pthread_create(&destroy_thread, NULL, destroy_target_worker, &race), 0);
	while (!__atomic_load_n(&blocking_cleanup_entered, __ATOMIC_ACQUIRE)) {
	}

	held = process_acquire(pid);
	cr_assert_eq(held, target, "target must still be present while process cleanup is blocked");
	cr_assert_eq(process_get_state(held),
	             PROCESS_STATE_EXITING,
	             "direct destruction must close capability admission through process state");
	process_release(held);
	cr_assert_eq(cap_create(grant_object_id, pid, CAP_READ, NULL),
	             CAP_ID_INVALID,
	             "grant admission must close before process cleanup starts");
	cr_assert_eq(cap_object_grant_count(grant_object), 0u);

	__atomic_store_n(&blocking_cleanup_release, true, __ATOMIC_RELEASE);
	cr_assert_eq(pthread_join(destroy_thread, NULL), 0);
	cr_assert(race.destroyed);
	cr_assert_null(process_acquire(pid));

	blocking_cleanup_target = PROCESS_PID_INVALID;
	cr_assert(cap_object_destroy_with_id(cleanup_object_id));
	cr_assert(cap_object_destroy(grant_object));
	cap_object_release(grant_object);
}

Test(capability, delegation_to_closed_target_has_no_topology_or_lifecycle_side_effects) {
	struct cap_object* object;
	struct capability* source;
	struct process*    target = NULL;
	cap_object_id_t    object_id;
	process_id_t       pid;
	size_t             grants_before;

	cap_test_setup();
	cr_assert_eq(process_create(&target, NULL), PROCESS_OK);
	cr_assert_not_null(target);
	pid = process_pid(target);
	cr_assert(process_terminate(target, PROCESS_EXIT_SYSTEM_UNKNOWN));

	object_id = cap_object_create(0x404u, NULL, NULL);
	object    = cap_object_acquire(object_id);
	cr_assert_not_null(object);
	source = cap_lookup(cap_create(object_id, 1u, CAP_READ | CAP_DELEGATE | CAP_DELEGATE_PEER, NULL));
	cr_assert_not_null(source);
	grants_before = cap_object_grant_count(object);

	cr_assert_eq(cap_delegate_create(source, pid, CAP_READ, false), CAP_ID_INVALID);
	cr_assert_eq(cap_delegate_create(source, pid, CAP_READ, true), CAP_ID_INVALID);
	cr_assert_null(source->first_child);
	cr_assert_eq(cap_object_grant_count(object), grants_before);

	cr_assert(process_destroy(target));
	cr_assert(cap_destroy(source));
	cr_assert(cap_object_destroy(object));
	cap_object_release(object);
}
