#ifndef TEST_PROCESS_TEST_SUPPORT_H
#define TEST_PROCESS_TEST_SUPPORT_H

#include <base/heap.h>
#include <base/process.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#include "../mocks/hal/cpu_mock.h"
#include "../mocks/hal/userspace_mock.h"
#include "../vmm/test_support.h"

void                init_process_test_environment(void);
enum process_result create_process_with_main_thread(struct process**                   out_process,
                                                    const struct process_spawn_params* params);
void                terminate_main_thread(struct process* process);
void                terminate_process_thread(struct uthread* thread);

#endif
