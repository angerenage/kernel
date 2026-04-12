#include <core/kthread.h>
#include <core/sched.h>

struct thread* kthread_current(void) {
	return sched_current_thread();
}

bool kthread_create(struct thread* thread, const struct thread_create_params* params) {
	return thread_init(thread, params);
}

bool kthread_start(struct thread* thread) {
	return sched_make_runnable(thread);
}

void kthread_yield(void) {
	sched_yield();
}

bool kthread_join(struct thread* target, thread_exit_code_t* out_exit_code) {
	struct thread* current = kthread_current();

	if (target == NULL || current == NULL || target == current || thread_is_idle(target) ||
	    !thread_is_joinable(target)) {
		return false;
	}

	if (!thread_is_terminated(target)) {
		sched_block_current(&target->join_wait_queue, THREAD_BLOCK_JOIN);
		if (!thread_is_terminated(target)) return false;
	}

	if (out_exit_code != NULL) *out_exit_code = target->exit_code;
	return true;
}

bool kthread_detach(struct thread* target) {
	return thread_detach(target);
}

bool kthread_cancel(struct thread* target) {
	return thread_request_cancel(target);
}

void kthread_exit(thread_exit_code_t exit_code) {
	sched_exit_current(exit_code);
}
