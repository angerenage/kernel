#include <base/cap.h>
#include <base/framebuffer.h>
#include <base/math.h>
#include <base/vmm.h>
#include <runtime/diagnostic.h>
#include <system/capability.h>
#include <system/framebuffer.h>

syscall_status_t framebuffer_get_info(cap_id_t framebuffer_cap, struct framebuffer_info_response* out_info) {
	const struct framebuffer_info_request request = {.header = {.op = FRAMEBUFFER_OP_INFO}};
	syscall_result_t                      result;
	size_t                                expected_size;

	if (framebuffer_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(framebuffer_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_info == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_info);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(framebuffer_cap, &request, sizeof(request), out_info, sizeof(*out_info));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(FRAMEBUFFER_OP_INFO, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(*out_info) || out_info->width == 0u || out_info->height == 0u || out_info->pitch == 0u ||
	    out_info->bpp == 0u || (uint64_t)(size_t)out_info->pitch != out_info->pitch ||
	    (uint64_t)(size_t)out_info->height != out_info->height ||
	    mul_overflow_size((size_t)out_info->pitch, (size_t)out_info->height, &expected_size) ||
	    out_info->size != expected_size) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("FRAMEBUFFER_OP_INFO returned an invalid response");
		return SYSCALL_STATUS_FAILED;
	}
	return SYSCALL_STATUS_OK;
}

syscall_status_t framebuffer_map(cap_id_t framebuffer_cap, struct framebuffer_map_response* out_mapping) {
	const struct framebuffer_map_request request = {.header = {.op = FRAMEBUFFER_OP_MAP}};
	struct framebuffer_map_response      response;
	syscall_result_t                     result;

	if (framebuffer_cap == CAP_ID_INVALID) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(framebuffer_cap);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	if (out_mapping == NULL) {
		RUNTIME_DIAGNOSTIC_INVALID_PARAMETER(out_mapping);
		return SYSCALL_STATUS_BAD_ARGUMENT;
	}
	result = cap_call_syscall(framebuffer_cap, &request, sizeof(request), &response, sizeof(response));
	RUNTIME_DIAGNOSTIC_OPERATION_RESULT(FRAMEBUFFER_OP_MAP, result);
	if (result.status != SYSCALL_STATUS_OK) return result.status;
	if (result.value != sizeof(response) || response.mapping_cap == CAP_ID_INVALID ||
	    response.mapping.id != VMM_ID_INVALID || response.mapping.base == NULL || response.mapping.page_count == 0u ||
	    response.mapping.prot != (VMM_PROT_READ | VMM_PROT_WRITE) || response.mapping.guard_pages != 0u ||
	    response.data_offset >= VMM_PAGE_SIZE) {
		RUNTIME_DIAGNOSTIC_INVALID_STATE("FRAMEBUFFER_OP_MAP returned an invalid mapping");
		return SYSCALL_STATUS_FAILED;
	}
	*out_mapping = response;
	return SYSCALL_STATUS_OK;
}
