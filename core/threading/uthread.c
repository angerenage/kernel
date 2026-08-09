#include <base/thread.h>
#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/id_table.h>
#include <core/kthread.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/signal.h>
#include <core/spinlock.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/hcf.h>
#include <hal/userspace.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <stddef.h>
#include <stdint.h>

enum {
	UTHREAD_KERNEL_STACK_PAGES   = 4u,
	UTHREAD_UPCALL_STACK_PAGES   = 4u,
	UTHREAD_USER_STACK_ALIGNMENT = HAL_USERSPACE_STACK_ALIGNMENT,
};

#define UTHREAD_REAPER_MAX_CPUS 64u

struct uthread_reaper {
	struct thread_wait_queue wait_queue;
	struct uthread*          head;
	struct uthread*          tail;
	struct kthread*          thread;
	bool                     started;
	bool                     starting;
};

static struct uthread_reaper uthread_reapers[UTHREAD_REAPER_MAX_CPUS];
static struct spinlock       uthread_reaper_init_lock =
	SPINLOCK_INIT_CLASS("uthread_reaper_init", SPINLOCK_ORDER_SCHED, SPINLOCK_FLAG_IRQSAVE);
static bool            uthread_reapers_initialized;
static struct id_table uthread_table = {
	.lock    = SPINLOCK_INIT_CLASS("uthread_table", SPINLOCK_ORDER_ID_TABLE, SPINLOCK_FLAG_IRQSAVE),
	.next_id = 1u,
	.min_id  = 1u,
	.max_id  = UINT64_MAX,
};

static void uthread_reaper_entry(void* arg);

static void uthread_unregister_id(struct uthread* thread) {
	if (thread == NULL || thread->id == UTHREAD_ID_INVALID) return;

	(void)id_table_remove(&uthread_table, thread->id, NULL);
	thread->id = UTHREAD_ID_INVALID;
}

static void uthread_release_name(struct uthread* thread) {
	if (thread == NULL) return;

	free((void*)thread->thread.name);
	thread->thread.name = NULL;
}

static bool uthread_begin_destroy(struct uthread* thread) {
	uint32_t expected = 0u;

	if (thread == NULL) return false;
	if (!__atomic_compare_exchange_n(&thread->dying, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		return false;
	}

	signal_unregister_thread_receivers(thread);
	uthread_unregister_id(thread);
	return true;
}

static bool uthread_attach_process(struct uthread* thread, struct process* process) {
	struct irq_state state;
	struct uthread*  cursor;
	bool             attached = false;

	if (thread == NULL || process == NULL) return false;

	state  = spinlock_lock_irqsave(&process->lock);
	cursor = process->thread_head;
	while (cursor != NULL) {
		if (cursor == thread) {
			spinlock_unlock_irqrestore(&process->lock, state);
			return false;
		}
		cursor = cursor->process_next;
	}
	if ((process->state == PROCESS_STATE_NEW || process->state == PROCESS_STATE_RUNNING) &&
	    process->thread_count != SIZE_MAX && thread->process_next == NULL) {
		if (process->thread_tail == NULL) {
			process->thread_head = thread;
		}
		else {
			process->thread_tail->process_next = thread;
		}
		process->thread_tail = thread;
		if (process->main_thread == NULL) process->main_thread = thread;
		thread->process = process;
		process->thread_count++;
		process->state = PROCESS_STATE_RUNNING;
		attached       = true;
	}
	spinlock_unlock_irqrestore(&process->lock, state);
	return attached;
}

static void uthread_detach_process(struct uthread* thread) {
	struct process*  process;
	struct uthread*  previous = NULL;
	struct uthread*  current;
	struct irq_state state;

	if (thread == NULL || thread->process == NULL) return;

	process = thread->process;
	state   = spinlock_lock_irqsave(&process->lock);
	current = process->thread_head;
	while (current != NULL && current != thread) {
		previous = current;
		current  = current->process_next;
	}
	if (current != NULL) {
		if (previous == NULL) {
			process->thread_head = current->process_next;
		}
		else {
			previous->process_next = current->process_next;
		}
		if (process->thread_tail == current) process->thread_tail = previous;
		if (process->main_thread == current) process->main_thread = process->thread_head;
		current->process_next = NULL;
		current->process      = NULL;
		if (process->thread_count != 0u) process->thread_count--;
	}
	spinlock_unlock_irqrestore(&process->lock, state);
}

static void uthread_release_stacks(struct uthread* thread) {
	struct address_space* address_space;

	if (thread == NULL) return;

	address_space = process_address_space(thread->process);
	if (thread->upcall.stack_id != VMM_ID_INVALID && address_space != NULL) {
		(void)vmm_free(address_space, thread->upcall.stack_id);
		thread->upcall.stack_id = VMM_ID_INVALID;
	}
	thread->upcall.stack_top = 0u;
	if (thread->user_stack_id != VMM_ID_INVALID && address_space != NULL) {
		(void)vmm_free(address_space, thread->user_stack_id);
		thread->user_stack_id = VMM_ID_INVALID;
	}
	if (thread->kernel_stack_id != VMM_ID_INVALID) {
		(void)vmm_free(address_space_kernel(), thread->kernel_stack_id);
		thread->kernel_stack_id = VMM_ID_INVALID;
	}
}

static void uthread_free(struct uthread* thread) {
	bool heap_allocated;

	if (thread == NULL) return;

	heap_allocated = thread->heap_allocated;
	(void)uthread_destroy_cap_object(thread);
	uthread_release_stacks(thread);
	uthread_upcall_state_deinit(thread);
	uthread_detach_process(thread);
	uthread_release_name(thread);
	thread->process = NULL;
	if (heap_allocated) {
		free(thread);
		return;
	}

	memset(thread, 0, sizeof(*thread));
	thread->user_stack_id   = VMM_ID_INVALID;
	thread->kernel_stack_id = VMM_ID_INVALID;
	thread->upcall.stack_id = VMM_ID_INVALID;
	thread->cap_object_id   = CAP_OBJECT_ID_INVALID;
}

static void uthread_reapers_init_once(void) {
	struct irq_state state;

	state = spinlock_lock_irqsave(&uthread_reaper_init_lock);
	if (!uthread_reapers_initialized) {
		for (size_t i = 0; i < UTHREAD_REAPER_MAX_CPUS; i++) {
			thread_wait_queue_init(&uthread_reapers[i].wait_queue);
			uthread_reapers[i].head     = NULL;
			uthread_reapers[i].tail     = NULL;
			uthread_reapers[i].thread   = NULL;
			uthread_reapers[i].started  = false;
			uthread_reapers[i].starting = false;
		}
		uthread_reapers_initialized = true;
	}
	spinlock_unlock_irqrestore(&uthread_reaper_init_lock, state);
}

static struct uthread_reaper* uthread_reaper_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= UTHREAD_REAPER_MAX_CPUS) return NULL;

	uthread_reapers_init_once();
	return &uthread_reapers[cpu->index];
}

static bool uthread_cpu_online(const struct cpu* cpu) {
	return cpu != NULL && cpu_state_get(cpu) == CPU_STATE_ONLINE;
}

static void uthread_reap_callback(struct thread* thread, void* ctx) {
	struct uthread* target = (struct uthread*)ctx;

	if (target == NULL || thread != &target->thread) return;

	thread_mark_zombie(thread);
	process_notify_thread_exit(target->process, thread, thread->exit_code);
	(void)sched_wake_all(&thread->join_wait_queue);
	if (!thread_is_joinable(thread) && uthread_begin_destroy(target)) uthread_release(target);
}

static bool __attribute__((unused))
uthread_reaper_start_cpu(struct cpu* cpu) {
	struct uthread_reaper*    reaper;
	struct irq_state          state;
	struct kthread*           reaper_thread = NULL;
	enum kthread_spawn_result result;

	reaper = uthread_reaper_for_cpu(cpu);
	if (reaper == NULL || !uthread_cpu_online(cpu)) return false;

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

	result = kthread_spawn_on_cpu(&reaper_thread, "uthread/reaper", uthread_reaper_entry, reaper, cpu);

	state            = spinlock_lock_irqsave(&reaper->wait_queue.lock);
	reaper->starting = false;
	if (result == KTHREAD_SPAWN_OK) {
		reaper->thread  = reaper_thread;
		reaper->started = true;
	}
	spinlock_unlock_irqrestore(&reaper->wait_queue.lock, state);

	if (result != KTHREAD_SPAWN_OK) {
		(void)kthread_destroy(reaper_thread);
		return false;
	}
	return true;
}

static enum uthread_start_result uthread_start_prepared(struct uthread*                    thread,
                                                        const struct uthread_start_params* params, bool heap_allocated,
                                                        bool reap_on_exit);

static enum uthread_start_result
uthread_start_internal(struct uthread* thread, const struct uthread_start_params* params, bool heap_allocated) {
	if (thread == NULL || params == NULL) return UTHREAD_START_INVALID_ARGUMENTS;

	return uthread_start_prepared(thread, params, heap_allocated, true);
}

static enum uthread_start_result uthread_start_prepared(struct uthread*                    thread,
                                                        const struct uthread_start_params* params, bool heap_allocated,
                                                        bool reap_on_exit) {
	struct vmm_alloc_params user_stack_params = {
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct vmm_alloc_params upcall_stack_params = {
		.page_count  = UTHREAD_UPCALL_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_USER,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct vmm_alloc_params kernel_stack_params = {
		.page_count  = UTHREAD_KERNEL_STACK_PAGES,
		.align_pages = 1u,
		.prot        = VMM_PROT_READ | VMM_PROT_WRITE | VMM_PROT_GLOBAL,
		.kind        = VMM_KIND_STACK,
		.guard_pages = VMM_STACK_DEFAULT_GUARD_PAGES,
		.map_flags   = 0u,
	};
	struct thread_context        context;
	struct thread_context_params thread_params;
	struct address_space*        address_space;
	enum thread_init_result      init_result;
	void*                        user_stack_base   = NULL;
	void*                        upcall_stack_base = NULL;
	void*                        kernel_stack_base = NULL;
	size_t                       user_stack_pages;
	uintptr_t                    kernel_stack_top;
	uintptr_t                    initial_user_stack_top;
	uintptr_t                    user_arg = 0u;
	uthread_id_t                 id;
	enum id_table_result         id_result;
	char*                        name = NULL;

	if (thread == NULL || params == NULL || params->process == NULL || params->user_entry == 0u) {
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	if ((params->arg_data == NULL) != (params->arg_size == 0u) || params->arg_size > THREAD_START_ARG_MAX_SIZE) {
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	address_space = process_address_space(params->process);
	if (!address_space_is_initialized(address_space)) return UTHREAD_START_INVALID_ARGUMENTS;

	if (params->name != NULL) {
		name = strdup(params->name);
		if (name == NULL) return UTHREAD_START_NO_MEMORY;
	}

	*thread = (struct uthread){
		.process         = params->process,
		.thread          = {.name = name},
		.user_stack_id   = VMM_ID_INVALID,
		.kernel_stack_id = VMM_ID_INVALID,
		.reaper_next     = NULL,
		.cap_object_id   = CAP_OBJECT_ID_INVALID,
		.reference_count = 1u,
		.dying           = 0u,
		.heap_allocated  = heap_allocated,
	};
	if (!uthread_upcall_state_init(thread)) {
		uthread_release_name(thread);
		uthread_upcall_state_deinit(thread);
		return UTHREAD_START_NO_MEMORY;
	}
	id_result = id_table_alloc(&uthread_table, thread, &id);
	if (id_result != ID_TABLE_OK) {
		uthread_release_name(thread);
		uthread_upcall_state_deinit(thread);
		return id_result == ID_TABLE_NO_MEMORY ? UTHREAD_START_NO_MEMORY : UTHREAD_START_ID_EXHAUSTED;
	}
	thread->id = id;

	user_stack_pages = params->user_stack_pages != 0u ? params->user_stack_pages : UTHREAD_DEFAULT_USER_STACK_PAGES;
	user_stack_params.page_count = user_stack_pages;
	if (!vmm_alloc(address_space, &user_stack_params, &thread->user_stack_id, &user_stack_base)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_STACK_ALLOC_FAILED;
	}
	if (!vmm_alloc(address_space, &upcall_stack_params, &thread->upcall.stack_id, &upcall_stack_base)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_STACK_ALLOC_FAILED;
	}
	thread->upcall.stack_top = (uintptr_t)upcall_stack_base + UTHREAD_UPCALL_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE;
	if (!vmm_alloc(address_space_kernel(), &kernel_stack_params, &thread->kernel_stack_id, &kernel_stack_base)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_STACK_ALLOC_FAILED;
	}

	thread->user_stack_top = (uintptr_t)user_stack_base + user_stack_pages * (uintptr_t)PMM_PAGE_SIZE;
	kernel_stack_top       = (uintptr_t)kernel_stack_base + UTHREAD_KERNEL_STACK_PAGES * (uintptr_t)PMM_PAGE_SIZE;
	initial_user_stack_top = thread->user_stack_top;
	if (params->arg_size != 0u) {
		if (params->arg_size > initial_user_stack_top - (uintptr_t)user_stack_base) {
			uthread_release_name(thread);
			uthread_release_stacks(thread);
			uthread_upcall_state_deinit(thread);
			uthread_unregister_id(thread);
			return UTHREAD_START_INVALID_ARGUMENTS;
		}
		user_arg = (initial_user_stack_top - params->arg_size) & ~((uintptr_t)UTHREAD_USER_STACK_ALIGNMENT - 1u);
		if (user_arg <= (uintptr_t)user_stack_base ||
		    address_space_copy_to(address_space, user_arg, params->arg_data, params->arg_size) != ADDRESS_TRANSFER_OK) {
			uthread_release_name(thread);
			uthread_release_stacks(thread);
			uthread_upcall_state_deinit(thread);
			uthread_unregister_id(thread);
			return UTHREAD_START_INVALID_ARGUMENTS;
		}
		initial_user_stack_top = user_arg;
	}

	if (!hal_userspace_thread_context_init(
			&context, kernel_stack_top, params->user_entry, initial_user_stack_top, user_arg)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_CONTEXT_UNSUPPORTED;
	}

	thread_params = (struct thread_context_params){
		.name              = thread->thread.name,
		.kernel_stack_base = (uintptr_t)kernel_stack_base,
		.kernel_stack_top  = kernel_stack_top,
		.address_space     = address_space,
		.preferred_cpu     = params->preferred_cpu,
		.base_priority     = THREAD_PRIORITY_DEFAULT,
		.detached          = params->detached,
		.context           = &context,
		.entry             = NULL,
		.arg               = NULL,
	};
	init_result = thread_init_context(&thread->thread, &thread_params);
	if (init_result != THREAD_INIT_OK) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	if (!uthread_attach_process(thread, thread->process)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_unregister_id(thread);
		return UTHREAD_START_INVALID_ARGUMENTS;
	}
	thread->thread.owner_kind = THREAD_OWNER_UTHREAD;
	thread->thread.owner      = thread;
	if (reap_on_exit) thread_set_reap_callback(&thread->thread, uthread_reap_callback, thread);
	if (!sched_make_runnable(&thread->thread)) {
		uthread_release_name(thread);
		uthread_release_stacks(thread);
		uthread_upcall_state_deinit(thread);
		uthread_detach_process(thread);
		uthread_unregister_id(thread);
		thread->process = NULL;
		return UTHREAD_START_SCHEDULER_REJECTED;
	}

	return UTHREAD_START_OK;
}

enum uthread_start_result uthread_start(struct uthread* thread, const struct uthread_start_params* params) {
	return uthread_start_internal(thread, params, false);
}

enum uthread_start_result uthread_spawn_detached(struct uthread**                   out_thread,
                                                 const struct uthread_start_params* params) {
	struct uthread_start_params effective_params;
	struct uthread*             thread;
	enum uthread_start_result   result;

	if (out_thread == NULL || params == NULL) return UTHREAD_START_INVALID_ARGUMENTS;
	*out_thread = NULL;

	thread = malloc(sizeof(*thread));
	if (thread == NULL) return UTHREAD_START_NO_MEMORY;
	*thread = (struct uthread){
		.user_stack_id   = VMM_ID_INVALID,
		.kernel_stack_id = VMM_ID_INVALID,
		.upcall          = {.stack_id = VMM_ID_INVALID},
		.heap_allocated  = true,
	};

	effective_params          = *params;
	effective_params.detached = true;
	result                    = uthread_start_internal(thread, &effective_params, true);
	if (result != UTHREAD_START_OK) {
		uthread_free(thread);
		return result;
	}
	*out_thread = thread;
	return UTHREAD_START_OK;
}

uthread_id_t uthread_id(const struct uthread* thread) {
	return thread == NULL ? UTHREAD_ID_INVALID : thread->id;
}

struct uthread* uthread_from_thread(struct thread* thread) {
	if (thread == NULL || thread->owner_kind != THREAD_OWNER_UTHREAD) return NULL;

	return (struct uthread*)thread->owner;
}

struct uthread* uthread_current(void) {
	return uthread_from_thread(sched_current_thread());
}

struct uthread* uthread_lookup(uthread_id_t id) {
	return (struct uthread*)id_table_lookup(&uthread_table, id);
}

bool uthread_retain(struct uthread* thread) {
	uint64_t old_refcount;

	if (thread == NULL) return false;

	for (;;) {
		if (__atomic_load_n(&thread->dying, __ATOMIC_ACQUIRE) != 0u) return false;

		old_refcount = __atomic_load_n(&thread->reference_count, __ATOMIC_ACQUIRE);
		if (old_refcount == 0u || old_refcount == UINT64_MAX) return false;
		if (__atomic_compare_exchange_n(&thread->reference_count,
		                                &old_refcount,
		                                old_refcount + 1u,
		                                false,
		                                __ATOMIC_ACQ_REL,
		                                __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

static bool uthread_retain_callback(void* value, void* context) {
	(void)context;
	return uthread_retain((struct uthread*)value);
}

struct uthread* uthread_acquire(uthread_id_t id) {
	if (id == UTHREAD_ID_INVALID) return NULL;

	return (struct uthread*)id_table_lookup_retain(&uthread_table, id, uthread_retain_callback, NULL);
}

void uthread_release(struct uthread* thread) {
	uint64_t old_refcount;

	if (thread == NULL) return;

	for (;;) {
		old_refcount = __atomic_load_n(&thread->reference_count, __ATOMIC_ACQUIRE);
		if (old_refcount == 0u) hcf();
		if (__atomic_compare_exchange_n(&thread->reference_count,
		                                &old_refcount,
		                                old_refcount - 1u,
		                                false,
		                                __ATOMIC_ACQ_REL,
		                                __ATOMIC_ACQUIRE)) {
			break;
		}
	}

	if (old_refcount != 1u) return;
	if (__atomic_load_n(&thread->dying, __ATOMIC_ACQUIRE) == 0u || thread->id != UTHREAD_ID_INVALID) hcf();
	uthread_free(thread);
}

size_t uthread_count(void) {
	return id_table_count(&uthread_table);
}

bool uthread_detach(struct uthread* thread) {
	if (thread == NULL) return false;
	if (thread_is_terminated(&thread->thread)) return false;
	if (!thread_detach(&thread->thread)) return false;

	thread_set_reap_callback(&thread->thread, uthread_reap_callback, thread);
	return true;
}

bool uthread_deinit(struct uthread* thread) {
	if (thread == NULL) return false;
	if (!thread_is_reap_safe(&thread->thread) && thread->thread.state != THREAD_STATE_NEW) return false;
	if (!thread_is_joinable(&thread->thread)) return false;
	if (!uthread_begin_destroy(thread)) return false;

	uthread_release(thread);
	return true;
}

cap_object_id_t uthread_cap_object_id(const struct uthread* thread) {
	return thread == NULL ? CAP_OBJECT_ID_INVALID : thread->cap_object_id;
}

void uthread_set_cap_object_id(struct uthread* thread, cap_object_id_t id) {
	if (thread != NULL) thread->cap_object_id = id;
}

bool uthread_destroy_cap_object(struct uthread* thread) {
	cap_object_id_t id;

	if (thread == NULL) return false;
	id                    = thread->cap_object_id;
	thread->cap_object_id = CAP_OBJECT_ID_INVALID;
	return id != CAP_OBJECT_ID_INVALID && cap_object_destroy_with_id(id);
}

static struct uthread* uthread_reaper_dequeue(struct uthread_reaper* reaper) {
	struct irq_state state;
	struct uthread*  target;

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

static void uthread_reaper_entry(void* arg) {
	struct uthread_reaper* reaper = arg;

	thread_set_cancel_enabled(kthread_current(), false);
	for (;;) {
		struct uthread* target = uthread_reaper_dequeue(reaper);

		if (target != NULL) {
			thread_mark_zombie(&target->thread);
			process_notify_thread_exit(target->process, &target->thread, target->thread.exit_code);
			(void)sched_wake_all(&target->thread.join_wait_queue);

			if (!thread_is_joinable(&target->thread) && uthread_begin_destroy(target)) {
				uthread_release(target);
			}
			continue;
		}

		if (reaper != NULL) sched_block_current(&reaper->wait_queue, THREAD_BLOCK_WAIT_QUEUE);
		else sched_yield();
	}
}
