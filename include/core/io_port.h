#pragma once

#if !defined(PLATFORM_PC_X86_64) && !defined(IO_PORT_TEST)
#error "I/O port state is only available on x86_64"
#endif

#include <base/cap.h>
#include <hal/io_port.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct io_port_state;
struct process;

enum io_port_result {
	IO_PORT_OK = 0,
	IO_PORT_INVALID_ARGUMENTS,
	IO_PORT_NO_MEMORY,
	IO_PORT_ALREADY_MAPPED,
	IO_PORT_NOT_MAPPED,
};

/* Add one capability-authorized port range to a process. */
enum io_port_result io_port_map(struct process* process, cap_id_t capability, uint32_t base, uint32_t count);

/* Remove the port range authorized by one capability from a process. */
enum io_port_result io_port_unmap(struct process* process, cap_id_t capability);

/* Remove one capability's port mapping from the identified process. */
bool io_port_unmap_capability(process_id_t process, cap_id_t capability);

/* Release every mapping and restore the process to its allocation-free state. */
void io_port_process_deinit(struct process* process);

/* Select this process's permissions for the current CPU before returning to userspace. */
void io_port_process_load(struct process* process);

/* Return the number of port mappings currently owned by a process. */
size_t io_port_process_mapping_count(struct process* process);

/* Return the current generation of a process's port permissions. */
uint64_t io_port_process_generation(struct process* process);
