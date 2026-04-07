#include <core/cpu.h>
#include <core/sched.h>
#include <core/thread.h>
#include <hal/cpu.h>
#include <stdio.h>

#define SCHED_MAX_CPU_COUNT 64u
#define SCHED_IDLE_NAME_MAX 16u

struct sched_cpu_state {
	bool             present;
	struct run_queue run_queue;
	struct thread    idle_thread;
	char             idle_name[SCHED_IDLE_NAME_MAX];
};

static struct sched_cpu_state sched_cpu_state[SCHED_MAX_CPU_COUNT];

static struct sched_cpu_state* sched_state_for_cpu(const struct cpu* cpu) {
	if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return NULL;
	if (!sched_cpu_state[cpu->index].present) return NULL;
	return &sched_cpu_state[cpu->index];
}

bool sched_init(void) {
	size_t cpu_total = cpu_count();

	if (cpu_total == 0u || cpu_total > SCHED_MAX_CPU_COUNT) return false;

	for (size_t i = 0; i < SCHED_MAX_CPU_COUNT; i++) {
		sched_cpu_state[i] = (struct sched_cpu_state){0};
	}

	for (size_t i = 0; i < cpu_total; i++) {
		struct cpu*             cpu = cpu_by_index(i);
		struct sched_cpu_state* state;

		if (cpu == NULL || cpu->index >= SCHED_MAX_CPU_COUNT) return false;

		state          = &sched_cpu_state[cpu->index];
		state->present = true;
		run_queue_init(&state->run_queue);
		sprintf(state->idle_name, "idle/%zu", cpu->index);
		thread_init_idle(&state->idle_thread, cpu, state->idle_name);
		cpu->current_thread = NULL;
	}

	return true;
}

bool sched_start_cpu(struct cpu* cpu) {
	struct thread* idle = sched_idle_thread(cpu);

	if (idle == NULL) return false;
	sched_set_current(cpu, idle);
	return true;
}

void sched_set_current(struct cpu* cpu, struct thread* thread) {
	if (cpu == NULL) return;

	cpu->current_thread = thread;
	thread_mark_running(thread, cpu);
}

struct thread* sched_current_thread(void) {
	struct cpu* cpu = cpu_current();
	return cpu == NULL ? NULL : cpu->current_thread;
}

struct thread* sched_idle_thread(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	return state == NULL ? NULL : &state->idle_thread;
}

bool sched_enqueue(struct cpu* cpu, struct thread* thread) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);

	if (state == NULL || thread == NULL) return false;

	thread->cpu = cpu;
	return run_queue_enqueue(&state->run_queue, thread);
}

struct thread* sched_dequeue_next(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	struct thread*          thread;

	if (state == NULL) return NULL;

	thread = run_queue_dequeue(&state->run_queue);
	if (thread == NULL) return &state->idle_thread;

	thread_mark_running(thread, cpu);
	return thread;
}

size_t sched_run_queue_depth(struct cpu* cpu) {
	struct sched_cpu_state* state = sched_state_for_cpu(cpu);
	return state == NULL ? 0u : run_queue_depth(&state->run_queue);
}

void sched_enter_idle(void) {
	struct cpu* cpu = cpu_current();

	if (!sched_start_cpu(cpu)) {
		for (;;) {
			hal_cpu_park();
		}
	}

	for (;;) {
		hal_cpu_park();
	}
}
