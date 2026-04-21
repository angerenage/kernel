#include <base/time.h>
#include <core/kheap.h>
#include <core/kthread.h>
#include <core/lock.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <stddef.h>

static struct thread_wait_queue kthread_reaper_wait_queue = {
	.lock = SPINLOCK_INIT_CLASS("kthread_reaper_wait", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE),
};
static struct kthread* kthread_reaper_head;
static struct kthread* kthread_reaper_tail;
static bool            kthread_reaper_started;
static bool            kthread_reaper_starting;

static enum kthread_spawn_result kthread_spawn_internal(struct kthread** out_thread, const char* name,
                                                        thread_entry_t entry, void* arg, struct cpu* preferred_cpu,
                                                        bool detached, bool reap_on_exit);
static void                      kthread_reaper_entry(void* arg);

static enum kthread_spawn_result kthread_result_from_thread_result(enum thread_init_result result) {
	switch (result) {
	case THREAD_INIT_OK:
		return KTHREAD_SPAWN_OK;
	case THREAD_INIT_CONTEXT_UNSUPPORTED:
		return KTHREAD_SPAWN_CONTEXT_UNSUPPORTED;
	case THREAD_INIT_INVALID_STACK:
		return KTHREAD_SPAWN_STACK_ALLOC_FAILED;
	case THREAD_INIT_INVALID_ARGUMENTS:
	default:
		return KTHREAD_SPAWN_INVALID_ARGUMENTS;
	}
}

static void kthread_free(struct kthread* thread) {
	if (thread == NULL) return;

	if (thread->stack_id != VMM_ID_INVALID) {
		(void)vmm_free(thread->stack_id);
		thread->stack_id = VMM_ID_INVALID;
	}
	kfree(thread);
}

static void kthread_reap_callback(struct thread* thread, void* ctx) {
	struct kthread*  target = (struct kthread*)ctx;
	struct irq_state state;

	(void)thread;
	if (target == NULL) return;

	state               = spinlock_lock_irqsave(&kthread_reaper_wait_queue.lock);
	target->reaper_next = NULL;
	if (kthread_reaper_tail == NULL) {
		kthread_reaper_head = target;
	}
	else {
		kthread_reaper_tail->reaper_next = target;
	}
	kthread_reaper_tail = target;
	spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);

	(void)sched_wake_one(&kthread_reaper_wait_queue);
}

bool kthread_reaper_start(struct cpu* preferred_cpu) {
	struct irq_state          state;
	struct kthread*           reaper = NULL;
	enum kthread_spawn_result result;

	state = spinlock_lock_irqsave(&kthread_reaper_wait_queue.lock);
	if (kthread_reaper_started) {
		spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);
		return true;
	}
	if (kthread_reaper_starting) {
		spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);
		return false;
	}
	kthread_reaper_starting = true;
	spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);

	result = kthread_spawn_internal(&reaper, "kthread/reaper", kthread_reaper_entry, NULL, preferred_cpu, true, false);

	state                   = spinlock_lock_irqsave(&kthread_reaper_wait_queue.lock);
	kthread_reaper_starting = false;
	if (result == KTHREAD_SPAWN_OK) kthread_reaper_started = true;
	spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);

	if (result != KTHREAD_SPAWN_OK) {
		kthread_free(reaper);
		return false;
	}
	return true;
}

static enum kthread_spawn_result kthread_spawn_internal(struct kthread** out_thread, const char* name,
                                                        thread_entry_t entry, void* arg, struct cpu* preferred_cpu,
                                                        bool detached, bool reap_on_exit) {
	struct vmm_alloc_params stack_params = {
		.page_count  = KTHREAD_DEFAULT_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct kthread*             thread;
	struct thread_create_params create_params;
	enum thread_init_result     init_result;
	void*                       stack_base = NULL;
	uintptr_t                   stack_base_addr;

	if (entry == NULL) return KTHREAD_SPAWN_INVALID_ARGUMENTS;
	if (!detached && out_thread == NULL) return KTHREAD_SPAWN_INVALID_ARGUMENTS;

	thread = kmalloc(sizeof(*thread));
	if (thread == NULL) return KTHREAD_SPAWN_NO_MEMORY;

	*thread = (struct kthread){
		.stack_id    = VMM_ID_INVALID,
		.stack_pages = KTHREAD_DEFAULT_STACK_PAGES,
	};

	if (!vmm_alloc(&stack_params, &thread->stack_id, &stack_base)) {
		kthread_free(thread);
		return KTHREAD_SPAWN_STACK_ALLOC_FAILED;
	}

	stack_base_addr                 = (uintptr_t)stack_base;
	create_params                   = (struct thread_create_params){0};
	create_params.name              = name;
	create_params.entry             = entry;
	create_params.arg               = arg;
	create_params.kernel_stack_base = stack_base_addr;
	create_params.kernel_stack_top  = stack_base_addr + KTHREAD_DEFAULT_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE;
	create_params.preferred_cpu     = preferred_cpu;
	create_params.detached          = detached;
	init_result                     = thread_init_ex(&thread->thread, &create_params);
	if (init_result != THREAD_INIT_OK) {
		enum kthread_spawn_result result = kthread_result_from_thread_result(init_result);

		kthread_free(thread);
		return result;
	}

	if (reap_on_exit) thread_set_reap_callback(&thread->thread, kthread_reap_callback, thread);

	if (!sched_make_runnable(&thread->thread)) {
		kthread_free(thread);
		return KTHREAD_SPAWN_START_FAILED;
	}

	if (out_thread != NULL) *out_thread = thread;
	return KTHREAD_SPAWN_OK;
}

enum kthread_spawn_result kthread_spawn(struct kthread** out_thread, const char* name, thread_entry_t entry,
                                        void* arg) {
	return kthread_spawn_on_cpu(out_thread, name, entry, arg, NULL);
}

enum kthread_spawn_result kthread_spawn_on_cpu(struct kthread** out_thread, const char* name, thread_entry_t entry,
                                               void* arg, struct cpu* preferred_cpu) {
	return kthread_spawn_internal(out_thread, name, entry, arg, preferred_cpu, false, false);
}

enum kthread_spawn_result kthread_spawn_detached(const char* name, thread_entry_t entry, void* arg) {
	return kthread_spawn_detached_on_cpu(name, entry, arg, NULL);
}

enum kthread_spawn_result kthread_spawn_detached_on_cpu(const char* name, thread_entry_t entry, void* arg,
                                                        struct cpu* preferred_cpu) {
	if (!kthread_reaper_start(preferred_cpu)) return KTHREAD_SPAWN_REAPER_UNAVAILABLE;

	return kthread_spawn_internal(NULL, name, entry, arg, preferred_cpu, true, true);
}

static struct kthread* kthread_reaper_dequeue(void) {
	struct irq_state state;
	struct kthread*  target;

	state  = spinlock_lock_irqsave(&kthread_reaper_wait_queue.lock);
	target = kthread_reaper_head;
	if (target != NULL) {
		kthread_reaper_head = target->reaper_next;
		if (kthread_reaper_head == NULL) kthread_reaper_tail = NULL;
		target->reaper_next = NULL;
	}
	spinlock_unlock_irqrestore(&kthread_reaper_wait_queue.lock, state);
	return target;
}

static void kthread_reaper_entry(void* arg) {
	(void)arg;

	thread_set_cancel_enabled(kthread_current(), false);
	for (;;) {
		struct kthread* target = kthread_reaper_dequeue();

		if (target != NULL) {
			kthread_free(target);
			continue;
		}

		sched_block_current(&kthread_reaper_wait_queue, THREAD_BLOCK_WAIT_QUEUE);
	}
}

struct thread* kthread_current(void) {
	return sched_current_thread();
}

void kthread_testcancel(void) {
	if (thread_should_cancel(kthread_current())) kthread_exit(THREAD_EXIT_CODE_CANCELLED);
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

bool kthread_join(struct kthread* target, thread_exit_code_t* out_exit_code) {
	struct thread* thread;
	struct thread* current = kthread_current();

	kthread_testcancel();
	if (target == NULL || current == NULL) return false;

	thread = &target->thread;
	if (thread == current || thread_is_idle(thread) || !thread_is_joinable(thread)) return false;

	if (!thread_is_terminated(thread)) {
		sched_block_current(&thread->join_wait_queue, THREAD_BLOCK_JOIN);
		kthread_testcancel();
		if (!thread_is_terminated(thread)) return false;
	}

	if (out_exit_code != NULL) *out_exit_code = thread->exit_code;
	return true;
}

bool kthread_destroy(struct kthread* target) {
	if (target == NULL) return false;
	if (!thread_is_terminated(&target->thread) && target->thread.state != THREAD_STATE_NEW) return false;
	if (!thread_is_joinable(&target->thread)) return false;

	kthread_free(target);
	return true;
}

bool kthread_detach(struct kthread* target) {
	if (target == NULL) return false;
	if (!kthread_reaper_start(target->thread.preferred_cpu)) return false;
	if (!thread_detach(&target->thread)) return false;

	thread_set_reap_callback(&target->thread, kthread_reap_callback, target);
	return true;
}

bool kthread_cancel(struct kthread* target) {
	return target != NULL && thread_request_cancel(&target->thread);
}

void kthread_exit(thread_exit_code_t exit_code) {
	sched_exit_current(exit_code);
}
