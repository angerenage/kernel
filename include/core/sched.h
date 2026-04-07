#pragma once

#include <stdbool.h>
#include <stddef.h>

struct cpu;
struct thread;

bool           sched_init(void);
bool           sched_start_cpu(struct cpu* cpu);
void           sched_set_current(struct cpu* cpu, struct thread* thread);
struct thread* sched_current_thread(void);
struct thread* sched_idle_thread(struct cpu* cpu);
bool           sched_enqueue(struct cpu* cpu, struct thread* thread);
struct thread* sched_dequeue_next(struct cpu* cpu);
size_t         sched_run_queue_depth(struct cpu* cpu);
void           sched_enter_idle(void) __attribute__((noreturn));
