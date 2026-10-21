#include <base/address_space.h>
#include <base/cap.h>
#include <base/memory.h>
#include <base/syscall.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <system/capability.h>
#include <system/memory.h>
#include <system/process.h>
#include <system/time.h>
#include <uacpi/kernel_api.h>
#include <uacpi_host/platform.h>

#if defined(__x86_64__)
#include <base/io_port.h>
#include <system/io_port.h>
#endif

struct host_mapping {
	struct host_mapping* next;
	cap_id_t             mapping_cap;
	uintptr_t            physical_base;
	uintptr_t            virtual_base;
	size_t               mapped_size;
	size_t               references;
};

struct host_mapping_view {
	struct host_mapping_view* next;
	struct host_mapping*      mapping;
	uintptr_t                 physical_address;
	void*                     address;
	size_t                    size;
	size_t                    references;
};

struct host_semaphore {
	size_t units;
};

struct host_spinlock {
	bool locked;
};

#if defined(__x86_64__)
struct host_io_mapping {
	cap_id_t capability;
};
#endif

static cap_id_t host_memory_allocator_cap = CAP_ID_INVALID;
static cap_id_t host_address_space_cap    = CAP_ID_INVALID;
#if defined(__x86_64__)
static cap_id_t host_io_ports_cap = CAP_ID_INVALID;
#endif
static size_t                    host_mapping_granule;
static uint64_t                  host_thread_id;
static struct host_mapping*      host_mappings;
static struct host_mapping_view* host_mapping_views;

static bool align_up_size(size_t value, size_t alignment, size_t* out_value) {
	if (alignment == 0u || (alignment & (alignment - 1u)) != 0u || value > SIZE_MAX - (alignment - 1u)) return false;
	*out_value = (value + alignment - 1u) & ~(alignment - 1u);
	return true;
}

bool uacpi_host_platform_initialize(cap_id_t memory_allocator_cap, cap_id_t io_ports_cap) {
	struct memory_allocator_info allocator;
	struct self_info             self;

	if (memory_allocator_cap == CAP_ID_INVALID || host_memory_allocator_cap != CAP_ID_INVALID ||
	    memory_allocator_info(memory_allocator_cap, &allocator) != SYSCALL_STATUS_OK ||
	    allocator.physical_claim_granule == 0u ||
	    (allocator.physical_claim_granule & (allocator.physical_claim_granule - 1u)) != 0u ||
	    process_self_info(&self) != SYSCALL_STATUS_OK || self.address_space_cap == CAP_ID_INVALID ||
	    self.thread_id == 0u)
		return false;

	host_memory_allocator_cap = memory_allocator_cap;
	host_address_space_cap    = self.address_space_cap;
#if defined(__x86_64__)
	host_io_ports_cap = io_ports_cap;
#else
	(void)io_ports_cap;
#endif
	host_mapping_granule = allocator.physical_claim_granule;
	host_thread_id       = self.thread_id;
	return true;
}

void uacpi_host_platform_deinitialize(void) {
	while (host_mapping_views != NULL) {
		struct host_mapping_view* view = host_mapping_views;

		host_mapping_views = view->next;
		free(view);
	}
	while (host_mappings != NULL) {
		struct host_mapping* mapping = host_mappings;

		host_mappings = mapping->next;
		(void)mapping_unmap(mapping->mapping_cap);
		free(mapping);
	}
	host_memory_allocator_cap = CAP_ID_INVALID;
	host_address_space_cap    = CAP_ID_INVALID;
#if defined(__x86_64__)
	host_io_ports_cap = CAP_ID_INVALID;
#endif
	host_mapping_granule = 0u;
	host_thread_id       = 0u;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address) {
	(void)out_rsdp_address;
	return UACPI_STATUS_UNIMPLEMENTED;
}

void* uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length) {
	struct address_space_map_response mapped = {.mapping_cap = CAP_ID_INVALID};
	struct host_mapping*              record;
	struct host_mapping_view*         view;
	cap_id_t                          memory_cap = CAP_ID_INVALID;
	uintptr_t                         physical;
	uintptr_t                         aligned;
	size_t                            offset;
	size_t                            mapped_size;

	if (host_memory_allocator_cap == CAP_ID_INVALID || host_address_space_cap == CAP_ID_INVALID ||
	    host_mapping_granule == 0u || address > UINTPTR_MAX || length == 0u || length > SIZE_MAX)
		return UACPI_MAP_FAILED;
	physical = (uintptr_t)address;
	aligned  = physical & ~(host_mapping_granule - 1u);
	offset   = physical - aligned;
	if (length > SIZE_MAX - offset || !align_up_size(offset + length, host_mapping_granule, &mapped_size) ||
	    aligned > UINTPTR_MAX - mapped_size)
		return UACPI_MAP_FAILED;

	for (view = host_mapping_views; view != NULL; view = view->next) {
		if (view->physical_address == physical && view->size == length) {
			view->references++;
			return view->address;
		}
	}
	for (record = host_mappings; record != NULL; record = record->next) {
		if (physical >= record->physical_base) {
			size_t relative = physical - record->physical_base;

			if (relative <= record->mapped_size && length <= record->mapped_size - relative) break;
		}
	}

	if (record == NULL) {
		if (memory_allocator_claim_physical(
				host_memory_allocator_cap, aligned, mapped_size, MEMORY_TYPE_DEVICE, &memory_cap) != SYSCALL_STATUS_OK)
			return UACPI_MAP_FAILED;
		if (address_space_map(host_address_space_cap,
		                      memory_cap,
		                      MEMORY_ACCESS_READ | MEMORY_ACCESS_WRITE,
		                      0u,
		                      host_mapping_granule,
		                      0u,
		                      0u,
		                      &mapped) != SYSCALL_STATUS_OK ||
		    mapped.address == 0u) {
			(void)cap_drop(memory_cap);
			return UACPI_MAP_FAILED;
		}
		if (cap_drop(memory_cap) != SYSCALL_STATUS_OK) {
			(void)mapping_unmap(mapped.mapping_cap);
			return UACPI_MAP_FAILED;
		}
		record = calloc(1u, sizeof(*record));
		if (record == NULL) {
			(void)mapping_unmap(mapped.mapping_cap);
			return UACPI_MAP_FAILED;
		}
		record->mapping_cap   = mapped.mapping_cap;
		record->physical_base = aligned;
		record->virtual_base  = mapped.address;
		record->mapped_size   = mapped_size;
		record->next          = host_mappings;
		host_mappings         = record;
	}
	view = calloc(1u, sizeof(*view));
	if (view == NULL) {
		if (record->references == 0u) {
			host_mappings = record->next;
			(void)mapping_unmap(record->mapping_cap);
			free(record);
		}
		return UACPI_MAP_FAILED;
	}
	view->mapping          = record;
	view->physical_address = physical;
	view->address          = (void*)(record->virtual_base + (physical - record->physical_base));
	view->size             = length;
	view->references       = 1u;
	view->next             = host_mapping_views;
	host_mapping_views     = view;
	record->references++;
	return view->address;
}

void uacpi_kernel_unmap(void* address, uacpi_size length) {
	struct host_mapping_view** cursor = &host_mapping_views;

	while (*cursor != NULL) {
		struct host_mapping_view* view = *cursor;
		struct host_mapping*      record;

		if (view->address == address && view->size == length) {
			if (--view->references != 0u) return;
			record  = view->mapping;
			*cursor = view->next;
			free(view);
			if (--record->references == 0u) {
				struct host_mapping** mapping_cursor = &host_mappings;

				while (*mapping_cursor != record) mapping_cursor = &(*mapping_cursor)->next;
				*mapping_cursor = record->next;
				(void)mapping_unmap(record->mapping_cap);
				free(record);
			}
			return;
		}
		cursor = &view->next;
	}
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* message) {
	const char* name;

	switch (level) {
	case UACPI_LOG_ERROR:
		name = "error";
		break;
	case UACPI_LOG_WARN:
		name = "warn";
		break;
	case UACPI_LOG_INFO:
		name = "info";
		break;
	case UACPI_LOG_TRACE:
		name = "trace";
		break;
	case UACPI_LOG_DEBUG:
		name = "debug";
		break;
	default:
		name = "unknown";
		break;
	}

	printf("uacpi[%s]: %s", name, message);
}

void* uacpi_kernel_alloc(uacpi_size size) {
	return malloc(size);
}

void uacpi_kernel_free(void* memory) {
	free(memory);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
	uint64_t value = 0u;
	(void)monotonic_time_ns(&value);
	return value;
}

void uacpi_kernel_stall(uacpi_u8 microseconds) {
	uint64_t begin;
	uint64_t now;
	uint64_t duration = (uint64_t)microseconds * 1000u;

	if (monotonic_time_ns(&begin) != SYSCALL_STATUS_OK) return;
	do {
		if (monotonic_time_ns(&now) != SYSCALL_STATUS_OK) return;
	} while (now - begin < duration);
}

void uacpi_kernel_sleep(uacpi_u64 milliseconds) {
	while (milliseconds > SIZE_MAX) {
		(void)sleep_ms(SIZE_MAX);
		milliseconds -= SIZE_MAX;
	}
	(void)sleep_ms((size_t)milliseconds);
}

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial_units) {
	struct host_semaphore* semaphore = malloc(sizeof(*semaphore));
	if (semaphore != NULL) semaphore->units = initial_units;
	return semaphore;
}

void uacpi_kernel_free_semaphore(uacpi_handle handle) {
	free(handle);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
	return (uacpi_thread_id)(uintptr_t)host_thread_id;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
	return 0u;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
	(void)state;
}

uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle handle, uacpi_u16 timeout) {
	struct host_semaphore* semaphore = handle;
	uint64_t               begin     = uacpi_kernel_get_nanoseconds_since_boot();

	if (semaphore == NULL) return UACPI_STATUS_INVALID_ARGUMENT;
	for (;;) {
		size_t units = __atomic_load_n(&semaphore->units, __ATOMIC_ACQUIRE);
		while (units != 0u) {
			if (__atomic_compare_exchange_n(
					&semaphore->units, &units, units - 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
				return UACPI_STATUS_OK;
		}
		if (timeout == 0u) return UACPI_STATUS_TIMEOUT;
		if (timeout != UINT16_MAX && uacpi_kernel_get_nanoseconds_since_boot() - begin >= (uint64_t)timeout * 1000000u)
			return UACPI_STATUS_TIMEOUT;
		(void)sleep_ms(1u);
	}
}

void uacpi_kernel_signal_semaphore(uacpi_handle handle) {
	struct host_semaphore* semaphore = handle;
	if (semaphore != NULL) (void)__atomic_fetch_add(&semaphore->units, 1u, __ATOMIC_RELEASE);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* request) {
	(void)request;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle context, uacpi_handle* out_handle) {
	(void)irq;
	(void)handler;
	(void)context;
	(void)out_handle;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle handle) {
	(void)handler;
	(void)handle;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
	return calloc(1u, sizeof(struct host_spinlock));
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
	free(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
	struct host_spinlock* lock = handle;
	while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) (void)sched_yield();
	return 0u;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
	struct host_spinlock* lock = handle;
	(void)flags;
	__atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

uacpi_handle uacpi_kernel_create_work_item(void) {
	return malloc(1u);
}

void uacpi_kernel_free_work_item(uacpi_handle handle) {
	free(handle);
}

void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle work_item, uacpi_work_handler handler,
                                uacpi_handle context) {
	(void)type;
	(void)work_item;
	if (handler != NULL) handler(context);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle* out_handle) {
	(void)address;
	(void)out_handle;
	return UACPI_STATUS_NOT_FOUND;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
	(void)handle;
}

#define UACPI_PCI_READ(width, type)                                                                                    \
	uacpi_status uacpi_kernel_pci_read##width(uacpi_handle handle, uacpi_size offset, type* value) {                   \
		(void)handle;                                                                                                  \
		(void)offset;                                                                                                  \
		if (value != NULL) *value = (type) ~(type)0;                                                                   \
		return UACPI_STATUS_NOT_FOUND;                                                                                 \
	}
#define UACPI_PCI_WRITE(width, type)                                                                                   \
	uacpi_status uacpi_kernel_pci_write##width(uacpi_handle handle, uacpi_size offset, type value) {                   \
		(void)handle;                                                                                                  \
		(void)offset;                                                                                                  \
		(void)value;                                                                                                   \
		return UACPI_STATUS_NOT_FOUND;                                                                                 \
	}

UACPI_PCI_READ(8, uacpi_u8)
UACPI_PCI_READ(16, uacpi_u16)
UACPI_PCI_READ(32, uacpi_u32)
UACPI_PCI_WRITE(8, uacpi_u8)
UACPI_PCI_WRITE(16, uacpi_u16)
UACPI_PCI_WRITE(32, uacpi_u32)

#undef UACPI_PCI_READ
#undef UACPI_PCI_WRITE

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle* out_handle) {
#if defined(__x86_64__)
	struct host_io_mapping* mapping;
	struct io_port_info     available;
	cap_id_t                capability = CAP_ID_INVALID;

	if (out_handle == NULL || host_io_ports_cap == CAP_ID_INVALID || base > UINT32_MAX || length == 0u ||
	    length > UINT32_MAX || io_port_info(host_io_ports_cap, &available) != SYSCALL_STATUS_OK ||
	    base < available.base || base - available.base > available.count ||
	    length > available.count - (base - available.base))
		return UACPI_STATUS_INVALID_ARGUMENT;
	if (io_port_derive(host_io_ports_cap,
	                   (uint32_t)base - available.base,
	                   (uint32_t)length,
	                   CAP_CALL | CAP_READ | CAP_WRITE,
	                   &capability) != SYSCALL_STATUS_OK)
		return UACPI_STATUS_DENIED;
	mapping = malloc(sizeof(*mapping));
	if (mapping == NULL) {
		(void)cap_drop(capability);
		return UACPI_STATUS_OUT_OF_MEMORY;
	}
	mapping->capability = capability;
	*out_handle         = mapping;
	return UACPI_STATUS_OK;
#else
	(void)base;
	(void)length;
	(void)out_handle;
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
#if defined(__x86_64__)
	struct host_io_mapping* mapping = handle;
	if (mapping == NULL) return;
	(void)cap_drop(mapping->capability);
	free(mapping);
#else
	(void)handle;
#endif
}

static uacpi_status io_read(uacpi_handle handle, uacpi_size offset, uacpi_size width, uacpi_u32* out_value) {
#if defined(__x86_64__)
	struct host_io_mapping* mapping = handle;
	if (mapping == NULL || offset > UINT32_MAX || out_value == NULL) return UACPI_STATUS_INVALID_ARGUMENT;
	return io_port_read(mapping->capability, (uint32_t)offset, (enum io_port_width)width, out_value) ==
	               SYSCALL_STATUS_OK
			   ? UACPI_STATUS_OK
			   : UACPI_STATUS_INTERNAL_ERROR;
#else
	(void)handle;
	(void)offset;
	(void)width;
	(void)out_value;
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

static uacpi_status io_write(uacpi_handle handle, uacpi_size offset, uacpi_size width, uacpi_u32 value) {
#if defined(__x86_64__)
	struct host_io_mapping* mapping = handle;
	if (mapping == NULL || offset > UINT32_MAX) return UACPI_STATUS_INVALID_ARGUMENT;
	return io_port_write(mapping->capability, (uint32_t)offset, (enum io_port_width)width, value) == SYSCALL_STATUS_OK
			   ? UACPI_STATUS_OK
			   : UACPI_STATUS_INTERNAL_ERROR;
#else
	(void)handle;
	(void)offset;
	(void)width;
	(void)value;
	return UACPI_STATUS_UNIMPLEMENTED;
#endif
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8* value) {
	uacpi_u32    wide;
	uacpi_status status = io_read(handle, offset, 1u, &wide);
	if (status == UACPI_STATUS_OK) *value = (uacpi_u8)wide;
	return status;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16* value) {
	uacpi_u32    wide;
	uacpi_status status = io_read(handle, offset, 2u, &wide);
	if (status == UACPI_STATUS_OK) *value = (uacpi_u16)wide;
	return status;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32* value) {
	return io_read(handle, offset, 4u, value);
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
	return io_write(handle, offset, 1u, value);
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
	return io_write(handle, offset, 2u, value);
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
	return io_write(handle, offset, 4u, value);
}
