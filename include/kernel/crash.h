#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum kernel_crash_arch {
	KERNEL_CRASH_ARCH_X86_64 = 1,
	KERNEL_CRASH_ARCH_AARCH64,
	KERNEL_CRASH_ARCH_RISCV64,
	KERNEL_CRASH_ARCH_LOONGARCH64,
};

enum kernel_crash_cause {
	KERNEL_CRASH_CAUSE_EXCEPTION = 1,
	KERNEL_CRASH_CAUSE_UNEXPECTED_INTERRUPT,
	KERNEL_CRASH_CAUSE_PANIC,
};

enum kernel_panic_reason {
	KERNEL_PANIC_UNSPECIFIED = 0,
	KERNEL_PANIC_ASSERTION,
	KERNEL_PANIC_INVARIANT,
	KERNEL_PANIC_OUT_OF_MEMORY,
	KERNEL_PANIC_EXPLICIT,
};

enum kernel_crash_trace_flags {
	KERNEL_CRASH_TRACE_UNAVAILABLE = 1u << 0,
	KERNEL_CRASH_TRACE_INVALID     = 1u << 1,
	KERNEL_CRASH_TRACE_TRUNCATED   = 1u << 2,
};

enum kernel_crash_frame_layout {
	KERNEL_CRASH_FRAME_RECORD_AT_FP = 0,
	KERNEL_CRASH_FRAME_RECORD_BEFORE_FP,
};

enum {
	KERNEL_CRASH_SCHEMA_VERSION = 1u,
	KERNEL_CRASH_BUILD_ID_SIZE  = 20u,
	KERNEL_CRASH_GIT_ID_SIZE    = 41u,
	KERNEL_CRASH_MAX_REGISTERS  = 32u,
	KERNEL_CRASH_MAX_TRACE      = 8u,
	KERNEL_CRASH_MAX_CPUS       = 64u,
	KERNEL_CRASH_RENDER_SIZE    = 4096u,
};

struct kernel_crash_record {
	uint32_t                 schema_version;
	enum kernel_crash_arch   arch;
	enum kernel_crash_cause  cause;
	char                     git_commit[KERNEL_CRASH_GIT_ID_SIZE];
	uint8_t                  build_id[KERNEL_CRASH_BUILD_ID_SIZE];
	bool                     build_id_valid;
	uint64_t                 cpu_index;
	uint64_t                 processor_id;
	uint64_t                 arch_id;
	uint64_t                 cpu_count;
	uint64_t                 total_memory;
	uint64_t                 exception_code;
	uint64_t                 exception_subcode;
	uint64_t                 error_code;
	uint64_t                 fault_address;
	uint64_t                 instruction_pointer;
	uint64_t                 stack_pointer;
	uint64_t                 frame_pointer;
	uint64_t                 status;
	uint64_t                 registers[KERNEL_CRASH_MAX_REGISTERS];
	uint32_t                 register_count;
	uint64_t                 trace[KERNEL_CRASH_MAX_TRACE];
	uint32_t                 trace_count;
	uint32_t                 trace_flags;
	enum kernel_panic_reason panic_reason;
	uint64_t                 panic_detail;
};

struct kernel_crash_panic {
	enum kernel_panic_reason reason;
	uintptr_t                detail;
};

/* Reset a crash record and attach its immutable build identity. */
void kernel_crash_record_reset(struct kernel_crash_record* record, enum kernel_crash_arch arch,
                               enum kernel_crash_cause cause);

/* Extract a SHA-1 GNU build ID from a bounded ELF note region. */
bool kernel_crash_parse_build_id(const void* note, size_t note_size, uint8_t out_build_id[KERNEL_CRASH_BUILD_ID_SIZE]);

/* Collect a bounded frame-pointer trace from a validated stack range. */
void kernel_crash_walk_stack(struct kernel_crash_record* record, uintptr_t stack_base, uintptr_t stack_top,
                             enum kernel_crash_frame_layout layout);

/* Return the stable schema name for a saved architecture register. */
const char* kernel_crash_register_name(enum kernel_crash_arch arch, uint32_t index);

/* Render a crash record into a bounded readable key/value buffer. */
bool kernel_crash_render(const struct kernel_crash_record* record, char* output, size_t output_size);

/* Atomically claim an unowned crash-report slot for one-based CPU identity. */
bool kernel_crash_try_claim(uint32_t* owner, uint32_t candidate);

/* Claim the global crash report before reading any optional kernel state. */
struct kernel_crash_record* kernel_crash_begin(enum kernel_crash_arch arch, enum kernel_crash_cause cause);

/* Publish lock-free system metadata after CPU and memory discovery completes. */
void kernel_crash_publish_system_metadata(void);

/* Consume panic metadata recorded immediately before an intentional trap. */
bool kernel_crash_take_panic(struct kernel_crash_panic* panic);

/* Complete, render, and permanently halt after a claimed crash report. */
__attribute__((noreturn))
void kernel_crash_finish(struct kernel_crash_record* record);

/* Deliberately enters the architecture exception path to capture a full register frame. */
__attribute__((noreturn))
void kernel_panic(enum kernel_panic_reason reason, uintptr_t detail);
