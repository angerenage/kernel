#pragma once

#include <stdint.h>

typedef uint64_t process_id_t;

#define PROCESS_PID_INVALID ((process_id_t)0u)

/* Machine-word-sized exit code published by a process at termination. */
typedef uintptr_t process_exit_code_t;

/* Process exit codes grouped by family: each nibble reserves a class of faults/errors. */
enum process_exit_code {
	PROCESS_EXIT_SUCCESS = 0u,

	PROCESS_EXIT_SYSTEM_UNKNOWN = 0x100u,
	PROCESS_EXIT_SYSTEM_INVALID_STARTUP,
	PROCESS_EXIT_SYSTEM_RUNTIME_INIT_FAILED,

	/* Memory access faults. */
	PROCESS_EXIT_MEMORY_NOT_PRESENT = 0x200u,
	PROCESS_EXIT_MEMORY_PROTECTION,
	PROCESS_EXIT_MEMORY_INVALID,

	/* Arithmetic exceptions. */
	PROCESS_EXIT_ARITHMETIC_DIVIDE_BY_ZERO = 0x300u,
	PROCESS_EXIT_ARITHMETIC_OVERFLOW,
	PROCESS_EXIT_ARITHMETIC_BOUND_RANGE,

	/* Illegal or undefined instruction. */
	PROCESS_EXIT_INSTRUCTION_ILLEGAL = 0x400u,

	/* General privilege or protection violation. */
	PROCESS_EXIT_PRIVILEGE_GENERAL_PROTECTION = 0x500u,

	/* Data alignment fault. */
	PROCESS_EXIT_ALIGNMENT_FAULT = 0x600u,

	/* Access aborts (instruction/data) and address errors. */
	PROCESS_EXIT_ACCESS_INSTRUCTION_ABORT = 0x700u,
	PROCESS_EXIT_ACCESS_DATA_ABORT,
	PROCESS_EXIT_ACCESS_ADDRESS_ERROR_FETCH,
	PROCESS_EXIT_ACCESS_ADDRESS_ERROR_MEMORY,

	/* Bus error. */
	PROCESS_EXIT_BUS_ERROR = 0x800u,

	/* Floating-point and SIMD exceptions. */
	PROCESS_EXIT_FLOATING_POINT_ERROR = 0x900u,
	PROCESS_EXIT_FLOATING_POINT_SIMD,
};
