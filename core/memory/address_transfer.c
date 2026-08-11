#include <base/math.h>
#include <core/address_transfer.h>
#include <core/mm.h>
#include <core/pmm.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void* hhdm_phys_to_virt(uintptr_t phys) {
	return (void*)(uintptr_t)(phys + boot_info.direct_map_offset);
}

bool address_transfer_result_is_success(enum address_transfer_result result) {
	return result == ADDRESS_TRANSFER_OK;
}

static bool range_end(uintptr_t addr, size_t size, uintptr_t* out_end) {
	uint64_t end;

	if (out_end) *out_end = addr;
	if (size == 0u) return out_end != NULL;
	if (add_overflow_u64((uint64_t)addr, (uint64_t)size, &end)) return false;
	if (out_end) *out_end = (uintptr_t)end;
	return true;
}

static enum address_transfer_result prot_check(vmm_prot_t prot, uint32_t access) {
	if ((access & ADDRESS_TRANSFER_USER) != 0 && (prot & VMM_PROT_USER) == 0) return ADDRESS_TRANSFER_NOT_USER;
	if ((access & ADDRESS_TRANSFER_READ) != 0 && (prot & VMM_PROT_READ) == 0) return ADDRESS_TRANSFER_ACCESS_DENIED;
	if ((access & ADDRESS_TRANSFER_WRITE) != 0 && (prot & VMM_PROT_WRITE) == 0) return ADDRESS_TRANSFER_ACCESS_DENIED;
	if ((access & ADDRESS_TRANSFER_EXEC) != 0 && (prot & VMM_PROT_EXEC) == 0) return ADDRESS_TRANSFER_ACCESS_DENIED;
	return ADDRESS_TRANSFER_OK;
}

static enum address_transfer_result hal_flags_check(uint64_t flags, uint32_t access) {
	if ((access & ADDRESS_TRANSFER_USER) != 0 && (flags & HAL_PAGE_USER) == 0) return ADDRESS_TRANSFER_NOT_USER;
	if ((access & ADDRESS_TRANSFER_WRITE) != 0 && (flags & HAL_PAGE_WRITE) == 0) {
		return ADDRESS_TRANSFER_ACCESS_DENIED;
	}
	if ((access & ADDRESS_TRANSFER_EXEC) != 0 && (flags & HAL_PAGE_EXEC) == 0) {
		return ADDRESS_TRANSFER_ACCESS_DENIED;
	}
	return ADDRESS_TRANSFER_OK;
}

static enum address_transfer_result translate_page(const struct vmm_transfer_guard* guard, struct address_space* space,
                                                   uintptr_t addr, uint32_t access, uintptr_t* out_phys,
                                                   size_t* out_chunk) {
	struct vmm_info              allocation;
	uintptr_t                    page_base;
	uintptr_t                    page_offset;
	uintptr_t                    phys  = 0u;
	uint64_t                     flags = 0u;
	enum address_transfer_result access_result;

	if (out_phys) *out_phys = 0u;
	if (out_chunk) *out_chunk = 0u;
	if (space == NULL || !address_space_is_initialized(space)) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if ((access & ~(ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_EXEC | ADDRESS_TRANSFER_USER |
	                ADDRESS_TRANSFER_PRESENT | ADDRESS_TRANSFER_FAULT_IN)) != 0) {
		return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	}
	if ((access & ADDRESS_TRANSFER_FAULT_IN) != 0 && (access & ADDRESS_TRANSFER_PRESENT) != 0) {
		return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	}

	if (!vmm_query_guarded(guard, space, (void*)addr, &allocation)) return ADDRESS_TRANSFER_NOT_MAPPED;
	access_result = prot_check(allocation.prot, access);
	if (access_result != ADDRESS_TRANSFER_OK) return access_result;

	page_base   = addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1u);
	page_offset = addr & (uintptr_t)(PMM_PAGE_SIZE - 1u);
	if (!hal_paging_query(address_space_hal(space), page_base, &phys, &flags)) {
		if ((access & (ADDRESS_TRANSFER_PRESENT | ADDRESS_TRANSFER_FAULT_IN)) == 0) {
			if (out_chunk) *out_chunk = (size_t)PMM_PAGE_SIZE - (size_t)page_offset;
			return ADDRESS_TRANSFER_OK;
		}
		if ((access & ADDRESS_TRANSFER_FAULT_IN) == 0) return ADDRESS_TRANSFER_NOT_MAPPED;
		if (!vmm_resolve_page_fault_guarded(guard, space, addr)) return ADDRESS_TRANSFER_FAULT_FAILED;
		if (!hal_paging_query(address_space_hal(space), page_base, &phys, &flags)) {
			return ADDRESS_TRANSFER_FAULT_FAILED;
		}
	}

	access_result = hal_flags_check(flags, access);
	if (access_result != ADDRESS_TRANSFER_OK) return access_result;
	if (out_phys) *out_phys = phys + page_offset;
	if (out_chunk) *out_chunk = (size_t)PMM_PAGE_SIZE - (size_t)page_offset;
	return ADDRESS_TRANSFER_OK;
}

static enum address_transfer_result validate_range_guarded(const struct vmm_transfer_guard* guard,
                                                           struct address_space* space, uintptr_t addr, size_t size,
                                                           uint32_t access) {
	size_t checked = 0u;

	while (checked < size) {
		size_t                       chunk;
		enum address_transfer_result result = translate_page(guard, space, addr + checked, access, NULL, &chunk);

		if (result != ADDRESS_TRANSFER_OK) return result;
		if (chunk > size - checked) chunk = size - checked;
		checked += chunk;
	}
	return ADDRESS_TRANSFER_OK;
}

enum address_transfer_result address_space_validate_range(struct address_space* space, uintptr_t addr, size_t size,
                                                          uint32_t access) {
	uintptr_t                    end;
	struct vmm_transfer_guard    guard = {0};
	enum address_transfer_result result;

	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (!range_end(addr, size, &end)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	(void)end;

	vmm_transfer_guard_acquire(&guard);
	result = validate_range_guarded(&guard, space, addr, size, access);
	vmm_transfer_guard_release(&guard);
	return result;
}

enum address_transfer_result address_space_copy_from(struct address_space* src_space, uintptr_t src_addr, void* dst,
                                                     size_t size) {
	size_t                       copied = 0u;
	uintptr_t                    end;
	struct vmm_transfer_guard    guard = {0};
	enum address_transfer_result result;

	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (dst == NULL) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if (!range_end(src_addr, size, &end)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	(void)end;

	vmm_transfer_guard_acquire(&guard);
	result = validate_range_guarded(
		&guard, src_space, src_addr, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (result != ADDRESS_TRANSFER_OK) goto out;

	while (copied < size) {
		uintptr_t phys;
		size_t    chunk;
		result = translate_page(&guard,
		                        src_space,
		                        src_addr + copied,
		                        ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &phys,
		                        &chunk);

		if (result != ADDRESS_TRANSFER_OK) goto out;
		if (chunk > size - copied) chunk = size - copied;
		memcpy((uint8_t*)dst + copied, hhdm_phys_to_virt(phys), chunk);
		copied += chunk;
	}

	result = ADDRESS_TRANSFER_OK;
out:
	vmm_transfer_guard_release(&guard);
	return result;
}

enum address_transfer_result address_space_copy_to(struct address_space* dst_space, uintptr_t dst_addr, const void* src,
                                                   size_t size) {
	size_t                       copied = 0u;
	uintptr_t                    end;
	struct vmm_transfer_guard    guard = {0};
	enum address_transfer_result result;

	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (src == NULL) return ADDRESS_TRANSFER_INVALID_ARGUMENTS;
	if (!range_end(dst_addr, size, &end)) return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	(void)end;

	vmm_transfer_guard_acquire(&guard);
	result = validate_range_guarded(
		&guard, dst_space, dst_addr, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (result != ADDRESS_TRANSFER_OK) goto out;

	while (copied < size) {
		uintptr_t phys;
		size_t    chunk;
		result = translate_page(&guard,
		                        dst_space,
		                        dst_addr + copied,
		                        ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &phys,
		                        &chunk);

		if (result != ADDRESS_TRANSFER_OK) goto out;
		if (chunk > size - copied) chunk = size - copied;
		memcpy(hhdm_phys_to_virt(phys), (const uint8_t*)src + copied, chunk);
		copied += chunk;
	}

	result = ADDRESS_TRANSFER_OK;
out:
	vmm_transfer_guard_release(&guard);
	return result;
}

enum address_transfer_result address_space_copy_between(struct address_space* dst_space, uintptr_t dst_addr,
                                                        struct address_space* src_space, uintptr_t src_addr,
                                                        size_t size) {
	size_t                       copied = 0u;
	uintptr_t                    src_end;
	uintptr_t                    dst_end;
	struct vmm_transfer_guard    guard = {0};
	enum address_transfer_result result;
	bool                         backward;

	if (size == 0u) return ADDRESS_TRANSFER_OK;
	if (!range_end(src_addr, size, &src_end) || !range_end(dst_addr, size, &dst_end)) {
		return ADDRESS_TRANSFER_ADDRESS_OVERFLOW;
	}
	(void)dst_end;
	backward = dst_space == src_space && dst_addr > src_addr && dst_addr < src_end;

	vmm_transfer_guard_acquire(&guard);
	result = validate_range_guarded(
		&guard, src_space, src_addr, size, ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (result != ADDRESS_TRANSFER_OK) goto out;
	result = validate_range_guarded(
		&guard, dst_space, dst_addr, size, ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN);
	if (result != ADDRESS_TRANSFER_OK) goto out;

	while (!backward && copied < size) {
		uintptr_t src_phys;
		uintptr_t dst_phys;
		size_t    src_chunk;
		size_t    dst_chunk;
		size_t    chunk;

		result = translate_page(&guard,
		                        src_space,
		                        src_addr + copied,
		                        ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &src_phys,
		                        &src_chunk);
		if (result != ADDRESS_TRANSFER_OK) goto out;
		result = translate_page(&guard,
		                        dst_space,
		                        dst_addr + copied,
		                        ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &dst_phys,
		                        &dst_chunk);
		if (result != ADDRESS_TRANSFER_OK) goto out;

		chunk = src_chunk < dst_chunk ? src_chunk : dst_chunk;
		if (chunk > size - copied) chunk = size - copied;
		memmove(hhdm_phys_to_virt(dst_phys), hhdm_phys_to_virt(src_phys), chunk);
		copied += chunk;
	}

	while (backward && copied < size) {
		const size_t    remaining      = size - copied;
		const uintptr_t src_last       = src_addr + remaining - 1u;
		const uintptr_t dst_last       = dst_addr + remaining - 1u;
		const size_t    src_page_bytes = (size_t)(src_last & (PMM_PAGE_SIZE - 1u)) + 1u;
		const size_t    dst_page_bytes = (size_t)(dst_last & (PMM_PAGE_SIZE - 1u)) + 1u;
		size_t          chunk          = src_page_bytes < dst_page_bytes ? src_page_bytes : dst_page_bytes;
		uintptr_t       src_phys;
		uintptr_t       dst_phys;
		size_t          ignored_chunk;
		size_t          offset;

		if (chunk > remaining) chunk = remaining;
		offset = remaining - chunk;
		result = translate_page(&guard,
		                        src_space,
		                        src_addr + offset,
		                        ADDRESS_TRANSFER_READ | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &src_phys,
		                        &ignored_chunk);
		if (result != ADDRESS_TRANSFER_OK) goto out;
		result = translate_page(&guard,
		                        dst_space,
		                        dst_addr + offset,
		                        ADDRESS_TRANSFER_WRITE | ADDRESS_TRANSFER_USER | ADDRESS_TRANSFER_FAULT_IN,
		                        &dst_phys,
		                        &ignored_chunk);
		if (result != ADDRESS_TRANSFER_OK) goto out;
		memmove(hhdm_phys_to_virt(dst_phys), hhdm_phys_to_virt(src_phys), chunk);
		copied += chunk;
	}

	result = ADDRESS_TRANSFER_OK;
out:
	vmm_transfer_guard_release(&guard);
	return result;
}

enum address_transfer_result address_space_read_u8(struct address_space* space, uintptr_t addr, uint8_t* out) {
	return address_space_copy_from(space, addr, out, sizeof(*out));
}

enum address_transfer_result address_space_read_u16(struct address_space* space, uintptr_t addr, uint16_t* out) {
	return address_space_copy_from(space, addr, out, sizeof(*out));
}

enum address_transfer_result address_space_read_u32(struct address_space* space, uintptr_t addr, uint32_t* out) {
	return address_space_copy_from(space, addr, out, sizeof(*out));
}

enum address_transfer_result address_space_read_u64(struct address_space* space, uintptr_t addr, uint64_t* out) {
	return address_space_copy_from(space, addr, out, sizeof(*out));
}

enum address_transfer_result address_space_read_uintptr(struct address_space* space, uintptr_t addr, uintptr_t* out) {
	return address_space_copy_from(space, addr, out, sizeof(*out));
}

enum address_transfer_result address_space_write_u8(struct address_space* space, uintptr_t addr, uint8_t value) {
	return address_space_copy_to(space, addr, &value, sizeof(value));
}

enum address_transfer_result address_space_write_u16(struct address_space* space, uintptr_t addr, uint16_t value) {
	return address_space_copy_to(space, addr, &value, sizeof(value));
}

enum address_transfer_result address_space_write_u32(struct address_space* space, uintptr_t addr, uint32_t value) {
	return address_space_copy_to(space, addr, &value, sizeof(value));
}

enum address_transfer_result address_space_write_u64(struct address_space* space, uintptr_t addr, uint64_t value) {
	return address_space_copy_to(space, addr, &value, sizeof(value));
}

enum address_transfer_result address_space_write_uintptr(struct address_space* space, uintptr_t addr, uintptr_t value) {
	return address_space_copy_to(space, addr, &value, sizeof(value));
}
