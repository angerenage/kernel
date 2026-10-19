#include <kernel/crash.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel_config.h"

static uint32_t crash_read_u32(const uint8_t* bytes) {
	uint32_t value;
	memcpy(&value, bytes, sizeof(value));
	return value;
}

static size_t crash_align4(size_t value) {
	return (value + 3u) & ~(size_t)3u;
}

static const char* crash_arch_name(enum kernel_crash_arch arch) {
	switch (arch) {
	case KERNEL_CRASH_ARCH_X86_64:
		return "x86_64";
	case KERNEL_CRASH_ARCH_AARCH64:
		return "aarch64";
	case KERNEL_CRASH_ARCH_RISCV64:
		return "riscv64";
	case KERNEL_CRASH_ARCH_LOONGARCH64:
		return "loongarch64";
	default:
		return "unknown";
	}
}

static const char* crash_cause_name(enum kernel_crash_cause cause) {
	switch (cause) {
	case KERNEL_CRASH_CAUSE_EXCEPTION:
		return "exception";
	case KERNEL_CRASH_CAUSE_UNEXPECTED_INTERRUPT:
		return "unexpected-interrupt";
	case KERNEL_CRASH_CAUSE_PANIC:
		return "panic";
	default:
		return "unknown";
	}
}

static bool crash_append_format(char* output, size_t output_size, size_t* length, const char* format, ...) {
	va_list arguments;
	int     written;

	va_start(arguments, format);
	written = vsnprintf(output + *length, output_size - *length, format, arguments);
	va_end(arguments);
	if (written < 0 || (size_t)written >= output_size - *length) {
		output[output_size - 1u] = '\0';
		return false;
	}
	*length += (size_t)written;
	return true;
}

void kernel_crash_record_reset(struct kernel_crash_record* record, enum kernel_crash_arch arch,
                               enum kernel_crash_cause cause) {
	if (record == NULL) return;
	memset(record, 0, sizeof(*record));
	record->schema_version = KERNEL_CRASH_SCHEMA_VERSION;
	record->arch           = arch;
	record->cause          = cause;
	strlcpy(record->git_commit, KERNEL_GIT_COMMIT, sizeof(record->git_commit));
}

bool kernel_crash_parse_build_id(const void* note, size_t note_size, uint8_t out_build_id[KERNEL_CRASH_BUILD_ID_SIZE]) {
	const uint8_t* bytes  = note;
	size_t         cursor = 0u;

	if (bytes == NULL || out_build_id == NULL) return false;
	while (note_size - cursor >= 12u) {
		uint32_t namesz = crash_read_u32(bytes + cursor);
		uint32_t descsz = crash_read_u32(bytes + cursor + 4u);
		uint32_t type   = crash_read_u32(bytes + cursor + 8u);
		size_t   name_size;
		size_t   desc_size;

		cursor += 12u;
		name_size = crash_align4(namesz);
		desc_size = crash_align4(descsz);
		if (name_size > note_size - cursor) return false;
		if (desc_size > note_size - cursor - name_size) return false;
		if (namesz == 4u && descsz == KERNEL_CRASH_BUILD_ID_SIZE && type == 3u &&
		    memcmp(bytes + cursor, "GNU\0", 4u) == 0) {
			memcpy(out_build_id, bytes + cursor + name_size, KERNEL_CRASH_BUILD_ID_SIZE);
			return true;
		}
		cursor += name_size + desc_size;
	}
	return false;
}

void kernel_crash_walk_stack(struct kernel_crash_record* record, uintptr_t stack_base, uintptr_t stack_top,
                             enum kernel_crash_frame_layout layout) {
	uintptr_t fp;
	uint32_t  visited = 0u;

	if (record == NULL) return;
	record->trace_count = 0u;
	record->trace_flags = 0u;
	if (record->instruction_pointer != 0u) record->trace[record->trace_count++] = record->instruction_pointer;
	fp = (uintptr_t)record->frame_pointer;
	if (fp == 0u || stack_top <= stack_base) {
		record->trace_flags |= KERNEL_CRASH_TRACE_UNAVAILABLE;
		return;
	}

	while (record->trace_count < KERNEL_CRASH_MAX_TRACE && visited++ < KERNEL_CRASH_MAX_TRACE) {
		uintptr_t record_address;
		uintptr_t next_fp;
		uintptr_t return_pc;

		if ((fp & (sizeof(uintptr_t) - 1u)) != 0u) {
			record->trace_flags |= KERNEL_CRASH_TRACE_INVALID;
			return;
		}
		if (layout == KERNEL_CRASH_FRAME_RECORD_BEFORE_FP) {
			if (fp < 2u * sizeof(uintptr_t)) {
				record->trace_flags |= KERNEL_CRASH_TRACE_INVALID;
				return;
			}
			record_address = fp - 2u * sizeof(uintptr_t);
		}
		else {
			record_address = fp;
		}
		if (record_address < stack_base || record_address > stack_top ||
		    stack_top - record_address < 2u * sizeof(uintptr_t)) {
			record->trace_flags |= KERNEL_CRASH_TRACE_INVALID;
			return;
		}
		memcpy(&next_fp, (const void*)record_address, sizeof(next_fp));
		memcpy(&return_pc, (const void*)(record_address + sizeof(uintptr_t)), sizeof(return_pc));
		if (return_pc != 0u) record->trace[record->trace_count++] = return_pc;
		if (next_fp == 0u) return;
		if (next_fp <= fp) {
			record->trace_flags |= KERNEL_CRASH_TRACE_INVALID;
			return;
		}
		fp = next_fp;
	}
	record->trace_flags |= KERNEL_CRASH_TRACE_TRUNCATED;
}

const char* kernel_crash_register_name(enum kernel_crash_arch arch, uint32_t index) {
	static const char* const x86[] = {
		"rax", "rbx", "rcx", "rdx", "rbp", "rdi", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
	static const char* const aarch[] = {"x0",  "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9",  "x10",
	                                    "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19", "x20", "x21",
	                                    "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29", "x30"};
	static const char* const riscv[] = {"ra", "sp", "gp", "tp",  "t0",  "t1", "t2", "s0", "s1", "a0", "a1",
	                                    "a2", "a3", "a4", "a5",  "a6",  "a7", "s2", "s3", "s4", "s5", "s6",
	                                    "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
	static const char* const loong[] = {"r0", "ra", "tp", "sp", "a0", "a1", "a2", "a3", "a4", "a5", "a6",
	                                    "a7", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "r21",
	                                    "fp", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"};

	if (arch == KERNEL_CRASH_ARCH_X86_64 && index < sizeof(x86) / sizeof(x86[0])) return x86[index];
	if (arch == KERNEL_CRASH_ARCH_AARCH64 && index < sizeof(aarch) / sizeof(aarch[0])) return aarch[index];
	if (arch == KERNEL_CRASH_ARCH_RISCV64 && index < sizeof(riscv) / sizeof(riscv[0])) return riscv[index];
	if (arch == KERNEL_CRASH_ARCH_LOONGARCH64 && index < sizeof(loong) / sizeof(loong[0])) return loong[index];
	return "unknown";
}

bool kernel_crash_render(const struct kernel_crash_record* record, char* output, size_t output_size) {
	static const char hex[] = "0123456789abcdef";
	char              build_id[2u * KERNEL_CRASH_BUILD_ID_SIZE + 1u];
	size_t            length = 0u;
	uint32_t          register_count;
	uint32_t          trace_count;

	if (record == NULL || output == NULL || output_size == 0u) return false;
	output[0] = '\0';
	if (!crash_append_format(output, output_size, &length, "--- KERNEL CRASH FRAME v%u ---\n", record->schema_version))
		return false;
	if (!crash_append_format(output, output_size, &length, "build.git=%s\n", record->git_commit)) return false;
	if (record->build_id_valid) {
		for (size_t i = 0u; i < KERNEL_CRASH_BUILD_ID_SIZE; i++) {
			build_id[2u * i]      = hex[record->build_id[i] >> 4u];
			build_id[2u * i + 1u] = hex[record->build_id[i] & 0x0fu];
		}
		build_id[2u * KERNEL_CRASH_BUILD_ID_SIZE] = '\0';
	}
	else {
		strlcpy(build_id, "unknown", sizeof(build_id));
	}
	if (!crash_append_format(output, output_size, &length, "build.id=%s\n", build_id) ||
	    !crash_append_format(output, output_size, &length, "system.arch=%s\n", crash_arch_name(record->arch)) ||
	    !crash_append_format(
			output, output_size, &length, "system.cpu_index=%llu\n", (unsigned long long)record->cpu_index) ||
	    !crash_append_format(output,
	                         output_size,
	                         &length,
	                         "system.processor_id=0x%016llx\n",
	                         (unsigned long long)record->processor_id) ||
	    !crash_append_format(
			output, output_size, &length, "system.arch_id=0x%016llx\n", (unsigned long long)record->arch_id) ||
	    !crash_append_format(
			output, output_size, &length, "system.cpu_count=%llu\n", (unsigned long long)record->cpu_count) ||
	    !crash_append_format(
			output, output_size, &length, "system.memory_bytes=%llu\n", (unsigned long long)record->total_memory) ||
	    !crash_append_format(output, output_size, &length, "crash.cause=%s\n", crash_cause_name(record->cause)) ||
	    !crash_append_format(
			output, output_size, &length, "crash.code=0x%016llx\n", (unsigned long long)record->exception_code) ||
	    !crash_append_format(
			output, output_size, &length, "crash.subcode=0x%016llx\n", (unsigned long long)record->exception_subcode) ||
	    !crash_append_format(
			output, output_size, &length, "crash.error=0x%016llx\n", (unsigned long long)record->error_code) ||
	    !crash_append_format(
			output, output_size, &length, "crash.address=0x%016llx\n", (unsigned long long)record->fault_address) ||
	    !crash_append_format(
			output, output_size, &length, "crash.pc=0x%016llx\n", (unsigned long long)record->instruction_pointer) ||
	    !crash_append_format(
			output, output_size, &length, "crash.sp=0x%016llx\n", (unsigned long long)record->stack_pointer) ||
	    !crash_append_format(
			output, output_size, &length, "crash.fp=0x%016llx\n", (unsigned long long)record->frame_pointer) ||
	    !crash_append_format(
			output, output_size, &length, "crash.status=0x%016llx\n", (unsigned long long)record->status))
		return false;
	if (record->cause == KERNEL_CRASH_CAUSE_PANIC) {
		if (!crash_append_format(output, output_size, &length, "panic.reason=%u\n", (unsigned)record->panic_reason) ||
		    !crash_append_format(
				output, output_size, &length, "panic.detail=0x%016llx\n", (unsigned long long)record->panic_detail))
			return false;
	}
	register_count =
		record->register_count < KERNEL_CRASH_MAX_REGISTERS ? record->register_count : KERNEL_CRASH_MAX_REGISTERS;
	for (uint32_t i = 0u; i < register_count; i++)
		if (!crash_append_format(output,
		                         output_size,
		                         &length,
		                         "register.%s=0x%016llx\n",
		                         kernel_crash_register_name(record->arch, i),
		                         (unsigned long long)record->registers[i]))
			return false;
	if (!crash_append_format(output, output_size, &length, "trace.flags=0x%08x\n", record->trace_flags)) return false;
	trace_count = record->trace_count < KERNEL_CRASH_MAX_TRACE ? record->trace_count : KERNEL_CRASH_MAX_TRACE;
	for (uint32_t i = 0u; i < trace_count; i++)
		if (!crash_append_format(
				output, output_size, &length, "trace.%u=0x%016llx\n", i, (unsigned long long)record->trace[i]))
			return false;
	return crash_append_format(output, output_size, &length, "--- END KERNEL CRASH FRAME ---\n");
}

bool kernel_crash_try_claim(uint32_t* owner, uint32_t candidate) {
	uint32_t expected = 0u;

	if (owner == NULL || candidate == 0u) return false;
	return __atomic_compare_exchange_n(owner, &expected, candidate, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
