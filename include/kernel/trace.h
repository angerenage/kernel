#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef KERNEL_TRACE_ENABLED
#define KERNEL_TRACE_ENABLED 0
#endif

#define KERNEL_TRACE_MAGIC 0x313045434152544bull /* "KTRACE01" in little-endian byte order. */
#define KERNEL_TRACE_VERSION 1u
#define KERNEL_TRACE_MAX_CPUS 64u

struct thread;

enum kernel_trace_record_type {
	KERNEL_TRACE_RECORD_INSTANT = 0u,
	KERNEL_TRACE_RECORD_BEGIN,
	KERNEL_TRACE_RECORD_END,
	KERNEL_TRACE_RECORD_COUNTER,
};

enum kernel_trace_event {
	KERNEL_TRACE_EVENT_SCHED_SWITCH = 1u,
	KERNEL_TRACE_EVENT_SCHED_BLOCK,
	KERNEL_TRACE_EVENT_SCHED_WAKE,
	KERNEL_TRACE_EVENT_SYSCALL,
	KERNEL_TRACE_EVENT_CAP_CALL_PHASE,
	KERNEL_TRACE_EVENT_CAP_CALL_HANDLER,
	KERNEL_TRACE_EVENT_CAP_CALL_OPERATION,
	KERNEL_TRACE_EVENT_SEMAPHORE_PHASE,
	KERNEL_TRACE_EVENT_VM_PHASE,
	KERNEL_TRACE_EVENT_SCHED_PHASE,
};

enum kernel_trace_cap_call_phase {
	KERNEL_TRACE_CAP_CALL_CAP_ACQUIRE = 0u,
	KERNEL_TRACE_CAP_CALL_REQUEST_ALLOC,
	KERNEL_TRACE_CAP_CALL_REQUEST_COPY,
	KERNEL_TRACE_CAP_CALL_RESPONSE_VALIDATE,
	KERNEL_TRACE_CAP_CALL_BEGIN,
	KERNEL_TRACE_CAP_CALL_KERNEL_RESPONSE_ALLOC,
	KERNEL_TRACE_CAP_CALL_KERNEL_HANDLER,
	KERNEL_TRACE_CAP_CALL_KERNEL_RESPONSE_COPY,
	KERNEL_TRACE_CAP_CALL_ENDPOINT_RETAIN,
	KERNEL_TRACE_CAP_CALL_PENDING_CREATE,
	KERNEL_TRACE_CAP_CALL_REQUEST_ATTACH,
	KERNEL_TRACE_CAP_CALL_CHANNEL_ENQUEUE,
	KERNEL_TRACE_CAP_CALL_REPLY_WAIT,
	KERNEL_TRACE_CAP_CALL_PENDING_DESTROY,
	KERNEL_TRACE_CAP_CALL_WAIT_TRY_ACQUIRE,
	KERNEL_TRACE_CAP_CALL_WAIT_BLOCK,
	KERNEL_TRACE_CAP_CALL_WAIT_RESULT,
};

enum kernel_trace_cap_call_operation {
	KERNEL_TRACE_CAP_CALL_PROCESS_INFO = 0,
	KERNEL_TRACE_CAP_CALL_PROCESS_RUN,
	KERNEL_TRACE_CAP_CALL_PROCESS_WAIT,
	KERNEL_TRACE_CAP_CALL_PROCESS_DETACH,
	KERNEL_TRACE_CAP_CALL_PROCESS_KILL,
	KERNEL_TRACE_CAP_CALL_PROCESS_SPAWN_THREAD,
	KERNEL_TRACE_CAP_CALL_MAPPING_INFO,
	KERNEL_TRACE_CAP_CALL_MAPPING_PROTECT,
	KERNEL_TRACE_CAP_CALL_MAPPING_UNMAP,
	KERNEL_TRACE_CAP_CALL_MAPPING_SYNC,
	KERNEL_TRACE_CAP_CALL_MEMORY_INFO,
	KERNEL_TRACE_CAP_CALL_MEMORY_SLICE,
};

enum kernel_trace_semaphore_phase {
	KERNEL_TRACE_SEMAPHORE_CHECK = 0,
	KERNEL_TRACE_SEMAPHORE_QUEUE_LOCK,
	KERNEL_TRACE_SEMAPHORE_SCHED_BLOCK,
	KERNEL_TRACE_SEMAPHORE_CANCEL_CHECK,
};

enum kernel_trace_vm_phase {
	KERNEL_TRACE_VM_MAPPING_CAP_ACQUIRE = 0,
	KERNEL_TRACE_VM_MAPPING_ADDRESS_SPACE_UNMAP,
	KERNEL_TRACE_VM_MAPPING_OBJECT_DESTROY,
	KERNEL_TRACE_VM_ADDRESS_SPACE_HAL_UNMAP,
	KERNEL_TRACE_VM_ADDRESS_SPACE_REMOVE,
	KERNEL_TRACE_VM_ADDRESS_SPACE_RELEASE,
	KERNEL_TRACE_VM_PAGING_PREPARE,
	KERNEL_TRACE_VM_PAGING_CHANGE,
	KERNEL_TRACE_VM_PAGING_SHOOTDOWN,
	KERNEL_TRACE_VM_PAGING_COMMIT,
	KERNEL_TRACE_VM_PAGING_CHANGE_RECORD,
	KERNEL_TRACE_VM_PAGING_CHANGE_DESCEND,
	KERNEL_TRACE_VM_PAGING_CHANGE_EMPTY_SCAN,
	KERNEL_TRACE_VM_PAGING_CHANGE_RETIRE,
	KERNEL_TRACE_VM_TLB_SHOOTDOWN_LOCK,
	KERNEL_TRACE_VM_TLB_SHOOTDOWN_LOCAL,
	KERNEL_TRACE_VM_TLB_SHOOTDOWN_TARGETS,
	KERNEL_TRACE_VM_TLB_SHOOTDOWN_SEND,
	KERNEL_TRACE_VM_TLB_SHOOTDOWN_WAIT,
};

enum kernel_trace_sched_phase {
	KERNEL_TRACE_SCHED_BLOCK_PREPARE = 0,
	KERNEL_TRACE_SCHED_BLOCK_ENQUEUE,
	KERNEL_TRACE_SCHED_BLOCK_CANCEL_CHECK,
	KERNEL_TRACE_SCHED_BLOCK_QUEUE_UNLOCK,
	KERNEL_TRACE_SCHED_DISPATCH_FINISH_PREVIOUS,
	KERNEL_TRACE_SCHED_DISPATCH_SELECT,
	KERNEL_TRACE_SCHED_DISPATCH_ADDRESS_SPACE,
	KERNEL_TRACE_SCHED_DISPATCH_SWITCH,
	KERNEL_TRACE_SCHED_SWITCH_FP_SAVE,
	KERNEL_TRACE_SCHED_SWITCH_FP_RESTORE,
	KERNEL_TRACE_SCHED_SWITCH_IO_BITMAP,
	KERNEL_TRACE_SCHED_ADDRESS_SPACE_LOCK,
	KERNEL_TRACE_SCHED_ADDRESS_SPACE_TRACK,
	KERNEL_TRACE_SCHED_ADDRESS_SPACE_CR3,
};

struct kernel_trace_record {
	uint64_t timestamp;
	uint64_t arg0;
	uint64_t arg1;
	uint32_t event_id;
	uint16_t cpu;
	uint8_t  type;
	uint8_t  flags;
};

struct kernel_trace_cpu_descriptor {
	uint64_t offset;
	uint64_t capacity;
	uint64_t committed;
	uint64_t dropped;
};

struct kernel_trace_file_header {
	uint64_t                           magic;
	uint32_t                           version;
	uint32_t                           header_size;
	uint64_t                           total_size;
	uint64_t                           clock_frequency;
	uint32_t                           record_size;
	uint32_t                           cpu_count;
	uint64_t                           session_id;
	uint32_t                           flags;
	uint32_t                           reserved;
	uint64_t                           checksum;
	struct kernel_trace_cpu_descriptor cpus[KERNEL_TRACE_MAX_CPUS];
};

_Static_assert(sizeof(struct kernel_trace_record) == 32u, "trace record format must remain stable");
_Static_assert(offsetof(struct kernel_trace_file_header, checksum) == 56u, "trace checksum offset changed");

#if KERNEL_TRACE_ENABLED
bool     kernel_trace_init(void);
bool     kernel_trace_start_autostop(void);
void     kernel_trace_finalize(void);
uint64_t kernel_trace_thread_id(const struct thread* thread);
void     kernel_trace_emit(uint32_t event_id, enum kernel_trace_record_type type, uint64_t arg0, uint64_t arg1);
#else
static inline bool kernel_trace_init(void) {
	return false;
}
static inline bool kernel_trace_start_autostop(void) {
	return false;
}
static inline void kernel_trace_finalize(void) {
}
#define kernel_trace_thread_id(thread) ((uint64_t)(uintptr_t)(thread))
#define kernel_trace_emit(event_id, type, arg0, arg1)                                                                  \
	do {                                                                                                               \
		(void)sizeof(event_id);                                                                                        \
		(void)sizeof(type);                                                                                            \
		(void)sizeof(arg0);                                                                                            \
		(void)sizeof(arg1);                                                                                            \
	} while (0)
#endif
