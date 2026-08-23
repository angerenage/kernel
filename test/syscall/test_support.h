#ifndef TEST_SYSCALL_TEST_SUPPORT_H
#define TEST_SYSCALL_TEST_SUPPORT_H

#include <base/cap.h>
#include <base/channel.h>
#include <base/heap.h>
#include <base/process.h>
#include <base/syscall.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/capability_call.h>
#include <core/channel.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <core/vm_space.h>
#include <criterion/criterion.h>
#include <hal/clock.h>
#include <hal/cpu.h>
#include <hal/interrupts.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../mocks/hal/cpu_mock.h"
#include "../vmm/test_support.h"

struct syscall_test_cap_request {
	uint32_t value;
};

struct syscall_test_cap_response {
	uint32_t value;
};

void             syscall_test_init_scheduler(void);
void             syscall_test_reset_state(void);
void             syscall_test_init_process_environment(void);
struct process*  syscall_test_spawn_process(const char* name);
void             syscall_test_thread_entry(void* arg);
syscall_result_t syscall_test_cap_handler(const struct cap_request* request);

#endif
