#include <base/math.h>
#include <base/vmm.h>
#include <core/address_transfer.h>
#include <core/pmm.h>
#include <string.h>

#include "vm_space_internal.h"

bool address_transfer_result_is_success(enum address_transfer_result result) {
	return result == ADDRESS_TRANSFER_OK;
}

static bool range_end(uintptr_t address, size_t size, uintptr_t* out_end) {
	uint64_t end;
	if (size == 0u) {
		if (out_end != NULL) *out_end = address;
		return true;
	}
	if (add_overflow_u64(address, size, &end)) return false;
	if (out_end != NULL) *out_end = (uintptr_t)end;
	return true;
}

static enum address_transfer_result check_access(const struct address_space* space, const struct vm_mapping* mapping,
                                                 uint32_t access) {
	if ((access & ADDRESS_TRANSFER_USER) != 0u && space == vm_space_kernel()) return ADDRESS_TRANSFER_NOT_USER;
	if (!memory_object_can_transfer(mapping->memory)) return ADDRESS_TRANSFER_ACCESS_DENIED;
	if ((access & ADDRESS_TRANSFER_READ) != 0u && (mapping->prot & VMM_PROT_READ) == 0u)
		return ADDRESS_TRANSFER_ACCESS_DENIED;
	if ((access & ADDRESS_TRANSFER_WRITE) != 0u && (mapping->prot & VMM_PROT_WRITE) == 0u)
		return ADDRESS_TRANSFER_ACCESS_DENIED;
	if ((access & ADDRESS_TRANSFER_EXEC) != 0u && (mapping->prot & VMM_PROT_EXEC) == 0u)
		return ADDRESS_TRANSFER_ACCESS_DENIED;
	return ADDRESS_TRANSFER_OK;
}

static enum address_transfer_result locate_locked(struct address_space* space, uintptr_t address, uint32_t access,
                                                  struct vm_mapping** out_mapping, size_t* out_object_offset,
                                                  size_t* out_chunk) {
	struct vm_mapping*           mapping;
	enum address_transfer_result result;
	uintptr_t                    mapping_end;
	if (out_mapping != NULL) *out_mapping = NULL;
	if (out_object_offset != NULL) *out_object_offset = 0u;
	if (out_chunk != NULL) *out_chunk = 0u;
	if (!vm_space_is_initialized(space) ||
	    (access & ~(ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_EXEC | ADDRESS_TRANSFER_USER |
	                ADDRESS_TRANSFER_PRESENT | ADDRESS_TRANSFER_FAULT_IN)) != 0u ||
	    ((access & ADDRESS_TRANSFER_PRESENT) != 0u && (access & ADDRESS_TRANSFER_FAULT_IN) != 0u))
		return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	mapping = vm_mapping_find_locked(space, address);
	if (mapping == NULL) return ADDRESS_TRANSFER_NOT_MAPPED;
	result = check_access(space, mapping, access);
	if (result != ADDRESS_TRANSFER_OK) return result;
	if ((access & (ADDRESS_TRANSFER_PRESENT | ADDRESS_TRANSFER_FAULT_IN)) != 0u) {
		uintptr_t page = address & ~(uintptr_t)(VMM_PAGE_SIZE - 1u);
		if (!hal_paging_query(space->hal, page, NULL)) {
			if ((access & ADDRESS_TRANSFER_PRESENT) != 0u) return ADDRESS_TRANSFER_NOT_MAPPED;
			size_t    page_index = (address - mapping->base) / VMM_PAGE_SIZE;
			uintptr_t phys;
			if (!memory_object_resolve_page(mapping->memory, mapping->memory_page_offset + page_index, &phys) ||
			    !hal_paging_map(space->hal,
			                    &(const struct hal_paging_map_request){
									.virtual_address  = page,
									.physical_address = phys,
									.size             = VMM_PAGE_SIZE,
									.flags            = vm_mapping_hal_flags(space, mapping->prot),
									.memory_type      = memory_object_memory_type(mapping->memory),
								}))
				return ADDRESS_TRANSFER_FAULT_FAILED;
		}
	}
	mapping_end = mapping->base + mapping->page_count * (uintptr_t)VMM_PAGE_SIZE;
	if (out_mapping != NULL) *out_mapping = mapping;
	if (out_object_offset != NULL)
		*out_object_offset = mapping->memory_page_offset * VMM_PAGE_SIZE + (size_t)(address - mapping->base);
	if (out_chunk != NULL) *out_chunk = (size_t)(mapping_end - address);
	return ADDRESS_TRANSFER_OK;
}

static enum address_transfer_result validate_locked(struct address_space* space, uintptr_t address, size_t size,
                                                    uint32_t access) {
	size_t done = 0u;
	while (done < size) {
		size_t                       chunk;
		enum address_transfer_result result = locate_locked(space, address + done, access, NULL, NULL, &chunk);
		if (result != ADDRESS_TRANSFER_OK) return result;
		if (chunk > size - done) chunk = size - done;
		done += chunk;
	}
	return ADDRESS_TRANSFER_OK;
}

enum address_transfer_result address_space_validate_range(struct address_space* space, uintptr_t address, size_t size,
                                                          uint32_t access) {
	struct irq_state state;
	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (!range_end(address, size, NULL)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	if (!vm_space_is_initialized(space)) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	state                               = spinlock_lock_irqsave(&space->lock);
	enum address_transfer_result result = validate_locked(space, address, size, access);
	spinlock_unlock_irqrestore(&space->lock, state);
	return result;
}

enum address_transfer_result address_space_copy_from(struct address_space* space, uintptr_t address, void* dst,
                                                     size_t size) {
	struct irq_state state;
	size_t           done = 0u;
	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (dst == NULL || !vm_space_is_initialized(space)) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if (!range_end(address, size, NULL)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	state = spinlock_lock_irqsave(&space->lock);
	enum address_transfer_result result =
		validate_locked(space, address, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER);
	while (result == ADDRESS_TRANSFER_OK && done < size) {
		struct vm_mapping* mapping;
		size_t             object_offset, chunk;
		result = locate_locked(
			space, address + done, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER, &mapping, &object_offset, &chunk);
		if (chunk > size - done) chunk = size - done;
		if (result == ADDRESS_TRANSFER_OK &&
		    !memory_object_read(mapping->memory, object_offset, (uint8_t*)dst + done, chunk))
			result = ADDRESS_TRANSFER_FAULT_FAILED;
		done += chunk;
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return result;
}

enum address_transfer_result address_space_copy_to(struct address_space* space, uintptr_t address, const void* src,
                                                   size_t size) {
	struct irq_state state;
	size_t           done = 0u;
	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (src == NULL || !vm_space_is_initialized(space)) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if (!range_end(address, size, NULL)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	state = spinlock_lock_irqsave(&space->lock);
	enum address_transfer_result result =
		validate_locked(space, address, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER);
	while (result == ADDRESS_TRANSFER_OK && done < size) {
		struct vm_mapping* mapping;
		size_t             object_offset, chunk;
		result = locate_locked(
			space, address + done, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER, &mapping, &object_offset, &chunk);
		if (chunk > size - done) chunk = size - done;
		if (result == ADDRESS_TRANSFER_OK &&
		    !memory_object_write(mapping->memory, object_offset, (const uint8_t*)src + done, chunk))
			result = ADDRESS_TRANSFER_FAULT_FAILED;
		done += chunk;
	}
	spinlock_unlock_irqrestore(&space->lock, state);
	return result;
}

static void lock_spaces(struct address_space* a, struct address_space* b, struct irq_state* first_state,
                        struct address_space** first, struct address_space** second) {
	*first       = (uintptr_t)a < (uintptr_t)b ? a : b;
	*second      = (uintptr_t)a < (uintptr_t)b ? b : a;
	*first_state = spinlock_lock_irqsave(&(*first)->lock);
	if (*second != *first) spinlock_lock(&(*second)->lock);
}

static void unlock_spaces(struct address_space* first, struct address_space* second, struct irq_state state) {
	if (second != first) spinlock_unlock(&second->lock);
	spinlock_unlock_irqrestore(&first->lock, state);
}

enum address_transfer_result address_space_copy_between(struct address_space* dst_space, uintptr_t dst_address,
                                                        struct address_space* src_space, uintptr_t src_address,
                                                        size_t size) {
	uint8_t               buffer[256];
	struct address_space *first, *second;
	struct irq_state      state;
	bool                  backward;
	size_t                done = 0u;
	uintptr_t             src_end;
	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (!vm_space_is_initialized(src_space) || !vm_space_is_initialized(dst_space))
		return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if (!range_end(src_address, size, &src_end) || !range_end(dst_address, size, NULL))
		return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	lock_spaces(src_space, dst_space, &state, &first, &second);
	enum address_transfer_result result =
		validate_locked(src_space, src_address, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER);
	if (result == ADDRESS_TRANSFER_OK)
		result = validate_locked(dst_space, dst_address, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER);
	backward = src_space == dst_space && dst_address > src_address && dst_address < src_end;
	while (result == ADDRESS_TRANSFER_OK && done < size) {
		struct vm_mapping *src_mapping, *dst_mapping;
		size_t             src_offset, dst_offset, src_chunk, dst_chunk;
		size_t             chunk = size - done;
		if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
		if (backward) {
			uintptr_t src_last = src_address + size - done - 1u;
			uintptr_t dst_last = dst_address + size - done - 1u;
			result             = locate_locked(
                src_space, src_last, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER, &src_mapping, &src_offset, NULL);
			if (result != ADDRESS_TRANSFER_OK) break;
			result = locate_locked(
				dst_space, dst_last, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER, &dst_mapping, &dst_offset, NULL);
			if (result != ADDRESS_TRANSFER_OK) break;
			src_chunk = (size_t)(src_last - src_mapping->base) + 1u;
			dst_chunk = (size_t)(dst_last - dst_mapping->base) + 1u;
			if (chunk > src_chunk) chunk = src_chunk;
			if (chunk > dst_chunk) chunk = dst_chunk;
			src_offset -= chunk - 1u;
			dst_offset -= chunk - 1u;
		}
		else {
			result = locate_locked(src_space,
			                       src_address + done,
			                       ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER,
			                       &src_mapping,
			                       &src_offset,
			                       &src_chunk);
			if (result != ADDRESS_TRANSFER_OK) break;
			result = locate_locked(dst_space,
			                       dst_address + done,
			                       ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER,
			                       &dst_mapping,
			                       &dst_offset,
			                       &dst_chunk);
			if (result != ADDRESS_TRANSFER_OK) break;
			if (chunk > src_chunk) chunk = src_chunk;
			if (chunk > dst_chunk) chunk = dst_chunk;
		}
		if (!memory_object_read(src_mapping->memory, src_offset, buffer, chunk) ||
		    !memory_object_write(dst_mapping->memory, dst_offset, buffer, chunk)) {
			result = ADDRESS_TRANSFER_FAULT_FAILED;
			break;
		}
		done += chunk;
	}
	unlock_spaces(first, second, state);
	return result;
}

#define DEFINE_READ(name, type)                                                                                        \
	enum address_transfer_result name(struct address_space* space, uintptr_t addr, type* out) {                        \
		return address_space_copy_from(space, addr, out, sizeof(*out));                                                \
	}
#define DEFINE_WRITE(name, type)                                                                                       \
	enum address_transfer_result name(struct address_space* space, uintptr_t addr, type value) {                       \
		return address_space_copy_to(space, addr, &value, sizeof(value));                                              \
	}
DEFINE_READ(address_space_read_u8, uint8_t)
DEFINE_READ(address_space_read_u16, uint16_t)
DEFINE_READ(address_space_read_u32, uint32_t)
DEFINE_READ(address_space_read_u64, uint64_t)
DEFINE_READ(address_space_read_uintptr, uintptr_t)
DEFINE_WRITE(address_space_write_u8, uint8_t)
DEFINE_WRITE(address_space_write_u16, uint16_t)
DEFINE_WRITE(address_space_write_u32, uint32_t)
DEFINE_WRITE(address_space_write_u64, uint64_t)
DEFINE_WRITE(address_space_write_uintptr, uintptr_t)
