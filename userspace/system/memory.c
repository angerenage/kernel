#include <stdbool.h>
#include <stdint.h>
#include <system/capability.h>
#include <system/memory.h>

#include "syscall.h"

bool address_space_map(cap_id_t address_space_cap, cap_id_t allocation_cap, cap_id_t* out_mapping_cap) {
	const struct address_space_map_request request = {.header         = {.op = ADDRESS_SPACE_OP_MAP},
	                                                  .allocation_cap = allocation_cap};
	struct address_space_map_response      response;
	syscall_result_t                       result;

	if (out_mapping_cap == NULL) return false;
	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_mapping_cap = response.mapping_cap;
	return true;
}

bool address_space_map_at(cap_id_t address_space_cap, cap_id_t allocation_cap, uintptr_t address,
                          cap_id_t* out_mapping_cap) {
	const struct address_space_map_at_request request = {
		.header = {.op = ADDRESS_SPACE_OP_MAP_AT}, .allocation_cap = allocation_cap, .address = address};
	struct address_space_map_response response;
	syscall_result_t                  result;

	if (out_mapping_cap == NULL) return false;
	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_mapping_cap = response.mapping_cap;
	return true;
}

bool allocation_write(cap_id_t allocation_cap, uintptr_t dst_offset, const void* src, size_t size) {
	const struct allocation_copy_to_request request = {
		.header      = {.op = ALLOCATION_OP_COPY_TO},
		.src_address = (uintptr_t)src,
		.dst_offset  = dst_offset,
		.size        = size,
	};
	struct allocation_copy_response response;
	syscall_result_t                result;

	if (size != 0u && src == NULL) return false;
	result = cap_call_syscall(allocation_cap, &request, sizeof(request), &response, sizeof(response));
	return result.status == SYSCALL_STATUS_OK && result.value == sizeof(response) && response.bytes_copied == size;
}

bool allocation_read(cap_id_t allocation_cap, uintptr_t src_offset, void* dst, size_t size) {
	const struct allocation_copy_from_request request = {
		.header      = {.op = ALLOCATION_OP_COPY_FROM},
		.src_offset  = src_offset,
		.dst_address = (uintptr_t)dst,
		.size        = size,
	};
	struct allocation_copy_response response;
	syscall_result_t                result;

	if (size != 0u && dst == NULL) return false;
	result = cap_call_syscall(allocation_cap, &request, sizeof(request), &response, sizeof(response));
	return result.status == SYSCALL_STATUS_OK && result.value == sizeof(response) && response.bytes_copied == size;
}

bool mapping_get_info(cap_id_t mapping_cap, struct vmm_info* out_info) {
	const struct mapping_read_request request = {.header = {.op = MAPPING_OP_READ}};
	struct mapping_read_response      response;
	syscall_result_t                  result;

	if (out_info == NULL) return false;
	result = cap_call_syscall(mapping_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_info = response.info;
	return true;
}

bool mapping_unmap(cap_id_t mapping_cap) {
	const struct mapping_unmap_request request = {.header = {.op = MAPPING_OP_UNMAP}};
	syscall_result_t                   result;

	result = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool mapping_protect(cap_id_t mapping_cap, vmm_prot_t prot) {
	const struct mapping_protect_request request = {.header = {.op = MAPPING_OP_PROTECT}, .prot = prot};
	syscall_result_t                     result  = cap_call_syscall(mapping_cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool allocation_free(cap_id_t allocation_cap) {
	const struct allocation_free_request request = {.header = {.op = ALLOCATION_OP_FREE}};
	syscall_result_t                     result = cap_call_syscall(allocation_cap, &request, sizeof(request), NULL, 0u);
	return result.status == SYSCALL_STATUS_OK;
}

bool address_space_query(cap_id_t address_space_cap, vmm_id_t id, struct vmm_info* out_info) {
	const struct address_space_query_request request = {.header = {.op = ADDRESS_SPACE_OP_QUERY}, .id = id};
	struct address_space_query_response      response;
	syscall_result_t                         result;

	if (out_info == NULL) return false;
	result = cap_call_syscall(address_space_cap, &request, sizeof(request), &response, sizeof(response));
	if (result.status != SYSCALL_STATUS_OK || result.value != sizeof(response)) return false;
	*out_info = response.info;
	return true;
}

bool memory_allocate(size_t page_count, vmm_prot_t prot, enum vmm_kind kind, cap_id_t* out_allocation_cap) {
	syscall_result_t result;

	if (out_allocation_cap == NULL) return false;
	result = syscall(SYSCALL_MEMORY_ALLOCATE, (uintptr_t)page_count, (uintptr_t)prot, (uintptr_t)kind, 0u, 0u, 0u);
	if (result.status != SYSCALL_STATUS_OK) return false;
	*out_allocation_cap = (cap_id_t)result.value;
	return true;
}
