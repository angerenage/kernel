#include <base/time.h>
#include <core/cpu.h>
#include <core/kheap.h>
#include <core/kthread.h>
#include <core/lock.h>
#include <core/pmm.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/clock.h>
#include <stddef.h>

#define KTHREAD_REAPER_MAX_CPUS 64u

struct kthread_reaper {
	struct thread_wait_queue wait_queue;
	struct kthread*          head;
	struct kthread*          tail;
	struct kthread*          thread;
	bool                     started;
	bool                     starting;
};

static struct kthread_reaper kthread_reapers[KTHREAD_REAPER_MAX_CPUS];
static struct spinlock       kthread_reaper_init_lock =
	SPINLOCK_INIT_CLASS("kthread_reaper_init", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE);
static bool kthread_reapers_initialized;

static enum kthread_spawn_result kthread_spawn_internal(struct kthread** out_thread, const char* name,
                                                        thread_entry_t entry, void* arg, struct cpu* preferred_cpu,
                                                        bool detached, bool reap_on_exit);
static void                      kthread_reaper_entry(void* arg);

static bool kthread_join_target(struct kthread* target, struct thread** out_thread) {
	struct thread* current = kthread_current();
	struct thread* thread;

	if (out_thread != NULL) *out_thread = NULL;
	if (target == NULL || current == NULL) return false;

	thread = &target->thread;
	if (thread == current || thread_is_idle(thread) || !thread_is_joinable(thread)) return false;

	if (out_thread != NULL) *out_thread = thread;
	return true;
}

static bool kthread_join_publish_exit_code(struct thread* thread, thread_exit_code_t* out_exit_code) {
	if (!thread_is_terminated(thread)) return false;

	if (out_exit_code != NULL) *out_exit_code = thread->exit_code;
	return true;
}

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
		(void)vmm_free(address_space_kernel(), thread->stack_id);
		thread->stack_id = VMM_ID_INVALID;
	}
	kfree(thread);
}

static void kthread_reapers_init_once(void) {
	struct irq_state state;

	state = spinlock_lock_irqsave(&kthread_reaper_init_lock);
	if (!kthread_reapers_initialized) {
		for (size_t i = 0; i < KTHREAD_REAPER_MAX_CPUS; i++) {
			thread_wait_queue_init(&kthread_reapers[i].wait_queue);
			kthread_reapers[i].head     = NULL;
			kthread_reapers[i].tail     = NULL;
			kthread_reapers[i].thread   = NULL;
			kthread_reapers[i].started  = false;
			kthread_reapers[i].starting = false;
		}
		kthread_reapers_initialized = true;
	}
	spinlock_unlock_irqrestore(&kthread_reaper_init_lock, state);
}

static struct kthread_reaper* kthread_reaper_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= KTHREAD_REAPER_MAX_CPUS) return NULL;

	kthread_reapers_init_once();
	return &kthread_reapers[cpu->index];
}

static bool kthread_cpu_online(const struct cpu* cpu) {
	return cpu != NULL && cpu_state_get(cpu) == CPU_STATE_ONLINE;
}

static struct cpu* kthread_default_cpu(void) {
	struct cpu* cpu = cpu_current();

	if (kthread_cpu_online(cpu)) return cpu;

	cpu = cpu_bsp();
	if (kthread_cpu_online(cpu)) return cpu;

	for (size_t i = 0; i < cpu_count(); i++) {
		cpu = cpu_by_index(i);
		if (kthread_cpu_online(cpu)) return cpu;
	}

	return NULL;
}

static void kthread_reap_callback(struct thread* thread, void* ctx) {
	struct kthread_reaper* reaper;
	struct kthread*        target = (struct kthread*)ctx;
	struct irq_state       state;

	(void)thread;
	if (target == NULL) return;

	reaper = kthread_reaper_for_cpu(cpu_current());
	if (reaper == NULL || !reaper->started) return;

	state               = spinlock_lock_irqsave(&reaper->wait_queue.lock);
	target->reaper_next = NULL;
	if (reaper->tail == NULL) {
		reaper->head = target;
	}
	else {
		reaper->tail->reaper_next = target;
	}
	reaper->tail = target;
	spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);

	(void)sched_wake_one(&reaper->wait_queue);
}

static bool kthread_reaper_start_cpu(struct cpu* cpu) {
	struct kthread_reaper*    reaper;
	struct irq_state          state;
	struct kthread*           reaper_thread = NULL;
	enum kthread_spawn_result result;

	reaper = kthread_reaper_for_cpu(cpu);
	if (reaper == NULL || !kthread_cpu_online(cpu)) return false;

	for (;;) {
		state = spinlock_lock_irqsave(&reaper->wait_queue.lock);
		if (reaper->started) {
			spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);
			return true;
		}
		if (!reaper->starting) {
			reaper->starting = true;
			spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);
			break;
		}
		spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);
		spinlock_relax();
	}

	result = kthread_spawn_internal(&reaper_thread, "kthread/reaper", kthread_reaper_entry, reaper, cpu, true, false);

	state            = spinlock_lock_irqsave(&reaper->wait_queue.lock);
	reaper->starting = false;
	if (result == KTHREAD_SPAWN_OK) {
		reaper->thread  = reaper_thread;
		reaper->started = true;
	}
	spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);

	if (result != KTHREAD_SPAWN_OK) {
		kthread_free(reaper_thread);
		return false;
	}
	return true;
}

bool kthread_reaper_start(struct cpu* preferred_cpu) {
	if (preferred_cpu != NULL) return kthread_reaper_start_cpu(preferred_cpu);

	return kthread_reaper_start_cpu(kthread_default_cpu());
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

	if (!vmm_alloc(address_space_kernel(), &stack_params, &thread->stack_id, &stack_base)) {
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
	struct cpu* target_cpu = preferred_cpu != NULL ? preferred_cpu : kthread_default_cpu();

	if (!kthread_reaper_start_cpu(target_cpu)) return KTHREAD_SPAWN_REAPER_UNAVAILABLE;

	return kthread_spawn_internal(NULL, name, entry, arg, target_cpu, true, true);
}

static struct kthread* kthread_reaper_dequeue(struct kthread_reaper* reaper) {
	struct irq_state state;
	struct kthread*  target;

	if (reaper == NULL) return NULL;

	state  = spinlock_lock_irqsave(&reaper->wait_queue.lock);
	target = reaper->head;
	if (target != NULL) {
		reaper->head = target->reaper_next;
		if (reaper->head == NULL) reaper->tail = NULL;
		target->reaper_next = NULL;
	}
	spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);
	return target;
}

static void kthread_reaper_entry(void* arg) {
	struct kthread_reaper* reaper = arg;

	thread_set_cancel_enabled(kthread_current(), false);
	for (;;) {
		struct kthread* target = kthread_reaper_dequeue(reaper);

		if (target != NULL) {
			kthread_free(target);
			continue;
		}

		if (reaper != NULL) sched_block_current(&reaper->wait_queue, THREAD_BLOCK_WAIT_QUEUE);
		else sched_yield();
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

bool kthread_park(void) {
	struct thread*   current = kthread_current();
	struct irq_state wait_state;

	kthread_testcancel();
	if (current == NULL || thread_is_idle(current) || thread_is_terminated(current)) return false;

	wait_state = spinlock_lock_irqsave(&current->park_wait_queue.lock);
	if ((current->flags & THREAD_FLAG_PARK_PERMIT) != 0u) {
		current->flags &= ~THREAD_FLAG_PARK_PERMIT;
		spinlock_unlock_irqrestore(&current->park_wait_queue.lock, wait_state);
		kthread_testcancel();
		return true;
	}

	if (!sched_block_current_locked(&current->park_wait_queue, THREAD_BLOCK_PARKED, wait_state)) {
		kthread_testcancel();
		return false;
	}

	wait_state = spinlock_lock_irqsave(&current->park_wait_queue.lock);
	current->flags &= ~THREAD_FLAG_PARK_PERMIT;
	spinlock_unlock_irqrestore(&current->park_wait_queue.lock, wait_state);

	kthread_testcancel();
	return true;
}

bool kthread_unpark(struct kthread* target) {
	struct thread*   thread;
	struct irq_state wait_state;
	bool             wake_target;

	if (target == NULL) return false;

	thread = &target->thread;
	if (thread_is_idle(thread) || thread_is_terminated(thread)) return false;

	wait_state = spinlock_lock_irqsave(&thread->park_wait_queue.lock);
	thread->flags |= THREAD_FLAG_PARK_PERMIT;
	wake_target = thread->state == THREAD_STATE_BLOCKED && thread->block_reason == THREAD_BLOCK_PARKED &&
	              thread->blocked_queue == &thread->park_wait_queue &&
	              thread->wait_status == THREAD_WAIT_STATUS_PENDING;
	spinlock_unlock_irqrestore(&thread->park_wait_queue.lock, wait_state);

	if (wake_target) (void)sched_wake_one(&thread->park_wait_queue);
	return true;
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

	kthread_testcancel();
	if (!kthread_join_target(target, &thread)) return false;

	if (!thread_is_terminated(thread)) {
		struct irq_state wait_state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);

		if (!thread_is_terminated(thread)) {
			if (!sched_block_current_locked(&thread->join_wait_queue, THREAD_BLOCK_JOIN, wait_state)) {
				kthread_testcancel();
				return false;
			}
		}
		else {
			spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, wait_state);
		}
		kthread_testcancel();
	}

	return kthread_join_publish_exit_code(thread, out_exit_code);
}

bool kthread_timed_join(struct kthread* target, uint64_t timeout_ms, thread_exit_code_t* out_exit_code) {
	struct thread* thread;
	uint64_t       deadline_tick;

	kthread_testcancel();
	if (!kthread_join_target(target, &thread)) return false;
	if (kthread_join_publish_exit_code(thread, out_exit_code)) return true;
	if (timeout_ms == 0u) return false;
	if (!time_tick_deadline_from_ms(sched_tick_count(), timeout_ms, hal_clock_frequency(), &deadline_tick)) {
		return false;
	}

	if (!thread_is_terminated(thread)) {
		struct irq_state wait_state = spinlock_lock_irqsave(&thread->join_wait_queue.lock);

		if (!thread_is_terminated(thread)) {
			if (!sched_block_current_until_locked(
					&thread->join_wait_queue, THREAD_BLOCK_JOIN, deadline_tick, wait_state)) {
				kthread_testcancel();
				return false;
			}
		}
		else {
			spinlock_unlock_irqrestore(&thread->join_wait_queue.lock, wait_state);
		}
		kthread_testcancel();
	}

	return kthread_join_publish_exit_code(thread, out_exit_code);
}

bool kthread_destroy(struct kthread* target) {
	if (target == NULL) return false;
	if (!thread_is_terminated(&target->thread) && target->thread.state != THREAD_STATE_NEW) return false;
	if (!thread_is_joinable(&target->thread)) return false;

	kthread_free(target);
	return true;
}

bool kthread_detach(struct kthread* target) {
	struct cpu* target_cpu;

	if (target == NULL) return false;
	target_cpu = kthread_cpu_online(target->thread.preferred_cpu) ? target->thread.preferred_cpu : target->thread.cpu;
	if (!kthread_cpu_online(target_cpu)) target_cpu = kthread_default_cpu();
	if (!kthread_reaper_start_cpu(target_cpu)) return false;

	target->thread.preferred_cpu = target_cpu;
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
