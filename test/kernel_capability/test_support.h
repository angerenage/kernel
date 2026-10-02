#pragma once

#include <base/cap.h>
#include <base/memory.h>
#include <base/module.h>
#include <boot/info.h>
#include <core/address_space.h>
#include <core/address_transfer.h>
#include <core/capability.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/process.h>
#include <core/sched.h>
#include <core/syscall.h>
#include <core/thread.h>
#include <core/uthread.h>
#include <criterion/criterion.h>
#include <hal/serial.h>
#include <kernel/capability.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../syscall/test_support.h"

struct kernel_capability_test_context {
	struct process* process;
	struct uthread* main_thread;
};

void kernel_capability_test_begin(struct kernel_capability_test_context* ctx, const char* name);
void kernel_capability_test_end(struct kernel_capability_test_context* ctx);

void   kernel_capability_test_serial_reset(void);
size_t kernel_capability_test_serial_bytes(void);

void boot_mock_set_modules(const struct boot_module* modules, size_t count);
void boot_mock_set_framebuffer(const struct boot_framebuffer* framebuffer);
void boot_mock_reset(void);

void acpi_mock_reset(void);
void acpi_mock_set_available(bool available);

syscall_result_t kernel_capability_test_call(cap_id_t cap, const void* request, size_t request_size, void* response,
                                             size_t response_capacity);

uintptr_t kernel_capability_test_alloc_user_buffer(struct process* process, size_t size, struct mapping** out_mapping);

void kernel_capability_test_poison_next_pmm_page(uint8_t value);
