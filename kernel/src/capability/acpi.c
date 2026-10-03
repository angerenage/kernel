#include "acpi.h"

#include <base/acpi.h>
#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <firmware/acpi.h>
#include <hal/acpi.h>
#include <kernel/capability.h>
#include <libc/stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ACPI_PROVIDER_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_MANAGE | CAP_DELEGATE))
#define ACPI_TABLE_RIGHTS ((cap_rights_t)(CAP_CALL | CAP_READ | CAP_DELEGATE))

static const struct hal_acpi_consumed_table acpi_hidden_tables[] = {
	{.signature = "FACP"},
	{.signature = "DSDT"},
	{.signature = "SSDT"},
	{.signature = "PSDT"},
	{.signature = "MSDM"},
	{.signature = "SLIC"},
	{.signature = "WPBT"},
	{.signature = "IBFT"},
	{.signature = "NBFT"},
	{.signature = "UEFI"},
};

static cap_object_id_t                 acpi_provider_object_id = CAP_OBJECT_ID_INVALID;
static struct hal_acpi_consumed_table* acpi_consumed_tables;
static size_t                          acpi_consumed_table_count;

#if defined(KERNEL_CAPABILITY_ACPI_TEST)
static bool     fail_next_claim_response;
static cap_id_t last_rollback_cap = CAP_ID_INVALID;

void kernel_capability_acpi_test_fail_next_claim_response(void) {
	fail_next_claim_response = true;
	last_rollback_cap        = CAP_ID_INVALID;
}

cap_id_t kernel_capability_acpi_test_last_rollback_cap(void) {
	return last_rollback_cap;
}
#endif

static bool acpi_signature_excluded(const char signature[4]) {
	if (signature == NULL) return true;
	for (size_t index = 0u; index < sizeof(acpi_hidden_tables) / sizeof(acpi_hidden_tables[0]); index++)
		if (memcmp(acpi_hidden_tables[index].signature, signature, 4u) == 0) return true;
	for (size_t index = 0u; index < acpi_consumed_table_count; index++)
		if (memcmp(acpi_consumed_tables[index].signature, signature, 4u) == 0) return true;
	return false;
}

static bool acpi_collect_consumed_tables(void) {
	size_t upper_bound = hal_acpi_consumed_tables(NULL, 0u);
	size_t allocation_size;
	size_t written;

	if (upper_bound == 0u) return true;
	if (upper_bound > SIZE_MAX / sizeof(*acpi_consumed_tables)) return false;
	allocation_size      = upper_bound * sizeof(*acpi_consumed_tables);
	acpi_consumed_tables = malloc(allocation_size);
	if (acpi_consumed_tables == NULL) return false;
	written = hal_acpi_consumed_tables(acpi_consumed_tables, upper_bound);
	if (written > upper_bound) {
		free(acpi_consumed_tables);
		acpi_consumed_tables = NULL;
		return false;
	}
	acpi_consumed_table_count = written;
	return true;
}

static const struct acpi_sdt_header* acpi_visible_table_at(const char signature[4], uint64_t target,
                                                           uint64_t* out_count) {
	acpi_cursor_t                 cursor = ACPI_CURSOR_INIT;
	uint64_t                      count  = 0u;
	const struct acpi_sdt_header* table;

	if (acpi_signature_excluded(signature)) {
		if (out_count != NULL) *out_count = 0u;
		return NULL;
	}
	while ((table = acpi_table_next(signature, &cursor)) != NULL) {
		if (count == target) return table;
		if (count == UINT64_MAX) break;
		count++;
	}
	if (out_count != NULL) *out_count = count;
	return NULL;
}

static size_t acpi_table_body_size(const struct acpi_sdt_header* table) {
	uint32_t length;

	if (table == NULL) return 0u;
	memcpy(&length, &table->length, sizeof(length));
	return length >= sizeof(*table) ? (size_t)length - sizeof(*table) : 0u;
}

static syscall_result_t acpi_table_info_handler(const struct cap_request* req, const struct acpi_sdt_header* table) {
	struct acpi_table_info_request  request;
	struct acpi_table_info_response response = {0};

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != ACPI_TABLE_OP_INFO) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	response.body_size = acpi_table_body_size(table);
	response.revision  = table->revision;
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t acpi_table_read_handler(const struct cap_request* req, const struct acpi_sdt_header* table) {
	struct acpi_table_read_request request;
	size_t                         body_size;
	size_t                         offset;
	size_t                         size;

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != ACPI_TABLE_OP_READ || request.reserved != 0u || request.offset > SIZE_MAX ||
	    request.size > SIZE_MAX || request.size > CAP_MAX_RESPONSE_SIZE)
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	body_size = acpi_table_body_size(table);
	offset    = (size_t)request.offset;
	size      = (size_t)request.size;
	if (offset > body_size || size > body_size - offset) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	if (size == 0u) return syscall_result_ok(0u);
	if (!cap_kernel_response_fits(req, size)) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	return cap_kernel_write_response(req, (const uint8_t*)table + sizeof(*table) + offset, size);
}

static syscall_result_t acpi_table_handler(const struct cap_request* req) {
	struct acpi_table_request_header header;
	const struct acpi_sdt_header*    table;

	if (req == NULL || req->object_id == 0u || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	table = (const struct acpi_sdt_header*)(uintptr_t)req->object_id;
	switch (header.op) {
	case ACPI_TABLE_OP_INFO:
		return acpi_table_info_handler(req, table);
	case ACPI_TABLE_OP_READ:
		return acpi_table_read_handler(req, table);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

static void acpi_table_object_event(struct cap_object* object, enum cap_object_event event) {
	if (event == CAP_OBJECT_EVENT_ZERO_GRANTS) (void)cap_object_destroy_if_unused(object);
}

static syscall_result_t acpi_provider_count_handler(const struct cap_request* req) {
	struct acpi_provider_count_request  request;
	struct acpi_provider_count_response response = {0};

	if ((req->rights & CAP_READ) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != ACPI_PROVIDER_OP_COUNT) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	(void)acpi_visible_table_at(request.signature, UINT64_MAX, &response.count);
	return cap_kernel_write_response(req, &response, sizeof(response));
}

static syscall_result_t acpi_write_claim_response(const struct cap_request*                  req,
                                                  const struct acpi_provider_claim_response* response) {
#if defined(KERNEL_CAPABILITY_ACPI_TEST)
	if (fail_next_claim_response) {
		fail_next_claim_response = false;
		last_rollback_cap        = response->table_cap;
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
#endif
	return cap_kernel_write_response(req, response, sizeof(*response));
}

static syscall_result_t acpi_provider_claim_handler(const struct cap_request* req) {
	struct acpi_provider_claim_request  request;
	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	const struct acpi_sdt_header*       table;
	cap_object_id_t                     object_id;
	bool                                created = false;
	syscall_result_t                    result;

	if ((req->rights & CAP_MANAGE) == 0u) return syscall_result_error(SYSCALL_STATUS_DENIED, 0u);
	if (req->request == NULL || req->request_size != sizeof(request) ||
	    !cap_kernel_response_fits(req, sizeof(response)))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&request, req->request, sizeof(request));
	if (request.header.op != ACPI_PROVIDER_OP_CLAIM) return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	table = acpi_visible_table_at(request.signature, request.index, NULL);
	if (table == NULL) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	object_id = cap_object_create_kernel_lifecycle(
		(uint64_t)(uintptr_t)table, acpi_table_handler, NULL, NULL, acpi_table_object_event, &created);
	if (object_id == CAP_OBJECT_ID_INVALID) return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	if (!created) return syscall_result_error(SYSCALL_STATUS_UNAVAILABLE, 0u);
	response.table_cap = cap_create(object_id, req->caller, ACPI_TABLE_RIGHTS, NULL);
	if (response.table_cap == CAP_ID_INVALID) {
		(void)cap_object_destroy_with_id(object_id);
		return syscall_result_error(SYSCALL_STATUS_FAILED, 0u);
	}
	result = acpi_write_claim_response(req, &response);
	if (result.status != SYSCALL_STATUS_OK) (void)cap_destroy_by_id(response.table_cap);
	return result;
}

static syscall_result_t acpi_provider_handler(const struct cap_request* req) {
	struct acpi_provider_request_header header;

	if (req == NULL || req->request == NULL || req->request_size < sizeof(header))
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	memcpy(&header, req->request, sizeof(header));
	switch (header.op) {
	case ACPI_PROVIDER_OP_COUNT:
		return acpi_provider_count_handler(req);
	case ACPI_PROVIDER_OP_CLAIM:
		return acpi_provider_claim_handler(req);
	default:
		return syscall_result_error(SYSCALL_STATUS_BAD_ARGUMENT, 0u);
	}
}

bool kernel_capability_acpi_init(void) {
	if (!acpi_available() || acpi_provider_object_id != CAP_OBJECT_ID_INVALID) return true;
	if (!acpi_collect_consumed_tables()) return false;
	acpi_provider_object_id = cap_object_create_kernel(0u, acpi_provider_handler, NULL);
	if (acpi_provider_object_id == CAP_OBJECT_ID_INVALID) {
		free(acpi_consumed_tables);
		acpi_consumed_tables      = NULL;
		acpi_consumed_table_count = 0u;
	}
	return acpi_provider_object_id != CAP_OBJECT_ID_INVALID;
}

bool kernel_capability_acpi_available(void) {
	return acpi_available() && acpi_provider_object_id != CAP_OBJECT_ID_INVALID;
}

cap_id_t kernel_capability_acpi_grant(process_id_t recipient) {
	if (!kernel_capability_acpi_available() || recipient == PROCESS_PID_INVALID) return CAP_ID_INVALID;
	return cap_create(acpi_provider_object_id, recipient, ACPI_PROVIDER_RIGHTS, NULL);
}
