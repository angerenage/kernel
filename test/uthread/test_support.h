#ifndef TEST_UTHREAD_TEST_SUPPORT_H
#define TEST_UTHREAD_TEST_SUPPORT_H

#include <base/heap.h>
#include <core/address_transfer.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vm_space.h>
#include <criterion/criterion.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <hal/userspace.h>

#include "../mocks/hal/cpu_mock.h"
#include "../mocks/hal/userspace_mock.h"
#include "../vmm/test_support.h"

void            init_uthread_test_environment(void);
struct process* spawn_owner_process(const char* name);
void            terminate_main_thread(struct process* process);

#endif
