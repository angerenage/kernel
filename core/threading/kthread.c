#include <base/time.h>
#include <core/kthread.h>
#include <core/sched.h>
#include <hal/clock.h>

struct thread* kthread_current(void) {
	return sched_current_thread();
}

enum thread_init_result kthread_create_ex(struct thread* thread, const struct thread_create_params* params) {
	return thread_init_ex(thread, params);
}

bool kthread_create(struct thread* thread, const struct thread_create_params* params) {
	return kthread_create_ex(thread, params) == THREAD_INIT_OK;
}

bool kthread_start(struct thread* thread) {
	return sched_make_runnable(thread);
}

void kthread_testcancel(void) {
	struct thread* current = kthread_current();

	if (!thread_should_cancel(current)) return;

	sched_exit_current(THREAD_EXIT_CODE_CANCELLED);
}

void kthread_yield(void) {
	kthread_testcancel();
	sched_yield();
	kthread_testcancel();
}

bool kthread_sleep_ms(uint64_t ms) {
	uint32_t timer_hz;
	uint64_t deadline_tick;

	kthread_testcancel();
	if (ms == 0u) {
		kthread_yield();
		return true;
	}

	timer_hz = hal_clock_frequency();
	if (!time_tick_deadline_from_ms(sched_tick_count(), ms, timer_hz, &deadline_tick)) return false;
	if (!sched_sleep_until_tick(deadline_tick)) return false;
	kthread_testcancel();
	return true;
}

bool kthread_join(struct thread* target, thread_exit_code_t* out_exit_code) {
	struct thread* current = kthread_current();

	kthread_testcancel();
	if (target == NULL || current == NULL || target == current || thread_is_idle(target) ||
	    !thread_is_joinable(target)) {
		return false;
	}

	if (!thread_is_terminated(target)) {
		sched_block_current(&target->join_wait_queue, THREAD_BLOCK_JOIN);
		kthread_testcancel();
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
