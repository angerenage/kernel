#include "../../kernel/src/capability/acpi.h"

#include <base/acpi.h>
#include <base/cap.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <criterion/criterion.h>
#include <firmware/acpi.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"

#define ACPI_TEST_TABLE_CAPACITY 8u
#define ACPI_TEST_BODY_CAPACITY 32u

struct acpi_test_table {
	struct acpi_sdt_header header;
	uint8_t                body[ACPI_TEST_BODY_CAPACITY];
};

ACPI_TABLE_EXCLUDE(KERN);

extern const char __start_acpi_excluded_tables[];
extern const char __stop_acpi_excluded_tables[];

static bool                          acpi_test_available;
static struct acpi_test_table        acpi_test_storage[ACPI_TEST_TABLE_CAPACITY];
static const struct acpi_sdt_header* acpi_test_tables[ACPI_TEST_TABLE_CAPACITY];
static size_t                        acpi_test_table_count;

void acpi_mock_reset(void) {
	acpi_test_available   = false;
	acpi_test_table_count = 0u;
	memset(acpi_test_storage, 0, sizeof(acpi_test_storage));
	memset(acpi_test_tables, 0, sizeof(acpi_test_tables));
}

void acpi_mock_set_available(bool available) {
	acpi_test_available = available;
}

bool acpi_available(void) {
	return acpi_test_available;
}

const struct acpi_sdt_header* acpi_table_next(const char signature[4], acpi_cursor_t* cursor) {
	acpi_cursor_t position = cursor == NULL ? ACPI_CURSOR_INIT : *cursor;

	if (!acpi_test_available || signature == NULL) return NULL;
	while (position < acpi_test_table_count) {
		const struct acpi_sdt_header* table = acpi_test_tables[position++];

		if (cursor != NULL) *cursor = position;
		if (memcmp(table->signature, signature, 4u) == 0) return table;
	}
	return NULL;
}

static const struct acpi_sdt_header* acpi_mock_add(const char signature[4], uint8_t revision, const void* body,
                                                   size_t body_size) {
	struct acpi_test_table* table;

	cr_assert_lt(acpi_test_table_count, ACPI_TEST_TABLE_CAPACITY);
	cr_assert_leq(body_size, ACPI_TEST_BODY_CAPACITY);
	table = &acpi_test_storage[acpi_test_table_count];
	memcpy(table->header.signature, signature, 4u);
	table->header.length   = (uint32_t)(sizeof(table->header) + body_size);
	table->header.revision = revision;
	if (body_size != 0u) memcpy(table->body, body, body_size);
	acpi_test_tables[acpi_test_table_count++] = &table->header;
	return &table->header;
}

static void acpi_mock_add_alias(const struct acpi_sdt_header* table) {
	cr_assert_not_null(table);
	cr_assert_lt(acpi_test_table_count, ACPI_TEST_TABLE_CAPACITY);
	acpi_test_tables[acpi_test_table_count++] = table;
}

static cap_id_t acpi_test_provider(struct kernel_capability_test_context* ctx) {
	cap_id_t provider;

	acpi_mock_set_available(true);
	cr_assert(kernel_capability_acpi_init());
	provider = kernel_capability_acpi_grant(process_pid(ctx->process));
	cr_assert_neq(provider, CAP_ID_INVALID);
	return provider;
}

static syscall_result_t acpi_test_count(cap_id_t provider, const char signature[4], uint64_t* count) {
	struct acpi_provider_count_request  request  = {.header = {.op = ACPI_PROVIDER_OP_COUNT}};
	struct acpi_provider_count_response response = {0};
	syscall_result_t                    result;

	memcpy(request.signature, signature, sizeof(request.signature));
	result = kernel_capability_test_call(provider, &request, sizeof(request), &response, sizeof(response));
	if (result.status == SYSCALL_STATUS_OK && count != NULL) *count = response.count;
	return result;
}

static syscall_result_t acpi_test_claim(cap_id_t provider, const char signature[4], uint64_t index,
                                        struct acpi_provider_claim_response* response) {
	struct acpi_provider_claim_request request = {.header = {.op = ACPI_PROVIDER_OP_CLAIM}, .index = index};

	memcpy(request.signature, signature, sizeof(request.signature));
	return kernel_capability_test_call(provider, &request, sizeof(request), response, sizeof(*response));
}

static cap_id_t acpi_test_claim_ok(cap_id_t provider, const char signature[4], uint64_t index) {
	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	syscall_result_t                    result   = acpi_test_claim(provider, signature, index, &response);

	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(response));
	cr_assert_neq(response.table_cap, CAP_ID_INVALID);
	return response.table_cap;
}

Test(kernel_capability_acpi, static_policy_blocks_owned_and_sensitive_tables) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[] = {1u, 2u, 3u};
	uint64_t                              count;

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-policy");
	acpi_mock_add("KERN", 1u, body, sizeof(body));
	acpi_mock_add("MSDM", 1u, body, sizeof(body));
	acpi_mock_add("TEST", 1u, body, sizeof(body));
	cap_id_t provider = acpi_test_provider(&ctx);

	cr_assert_eq((size_t)(__stop_acpi_excluded_tables - __start_acpi_excluded_tables) % 4u, 0u);
	cr_assert_eq(acpi_test_count(provider, "KERN", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 0u);
	cr_assert_eq(acpi_test_count(provider, "MSDM", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 0u);
	cr_assert_eq(acpi_test_count(provider, "NONE", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 0u);
	cr_assert_eq(acpi_test_count(provider, "TEST", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 1u);

	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	cr_assert_eq(acpi_test_claim(provider, "KERN", 0u, &response).status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(acpi_test_claim(provider, "NONE", 0u, &response).status, SYSCALL_STATUS_UNAVAILABLE);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, claims_are_exclusive_without_changing_count_or_indices) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         first_body[]  = {0x11u};
	const uint8_t                         second_body[] = {0x22u};
	uint64_t                              count;

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-exclusive");
	const struct acpi_sdt_header* first = acpi_mock_add("TEST", 1u, first_body, sizeof(first_body));
	acpi_mock_add("TEST", 2u, second_body, sizeof(second_body));
	acpi_mock_add_alias(first);
	cap_id_t provider  = acpi_test_provider(&ctx);
	cap_id_t first_cap = acpi_test_claim_ok(provider, "TEST", 0u);

	cr_assert_eq(acpi_test_count(provider, "TEST", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 3u);
	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	cr_assert_eq(acpi_test_claim(provider, "TEST", 0u, &response).status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert_eq(acpi_test_claim(provider, "TEST", 2u, &response).status, SYSCALL_STATUS_UNAVAILABLE);
	cap_id_t second_cap = acpi_test_claim_ok(provider, "TEST", 1u);
	cr_assert(cap_destroy_by_id(first_cap));
	cap_id_t reclaimed = acpi_test_claim_ok(provider, "TEST", 2u);
	cr_assert(cap_destroy_by_id(second_cap));
	cr_assert(cap_destroy_by_id(reclaimed));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, delegation_holds_claim_until_the_final_grant_is_dropped) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[] = {0x55u};

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-delegation");
	acpi_mock_add("TEST", 1u, body, sizeof(body));
	cap_id_t           provider = acpi_test_provider(&ctx);
	cap_id_t           original = acpi_test_claim_ok(provider, "TEST", 0u);
	struct capability* grant    = cap_acquire(original);
	cr_assert_not_null(grant);
	cap_id_t delegated =
		cap_delegate_create(grant, process_pid(ctx.process), CAP_CALL | CAP_READ | CAP_DELEGATE, false);
	cr_assert_neq(delegated, CAP_ID_INVALID);
	cr_assert(cap_drop(grant));
	cap_release(grant);

	struct acpi_provider_claim_response response = {.table_cap = CAP_ID_INVALID};
	cr_assert_eq(acpi_test_claim(provider, "TEST", 0u, &response).status, SYSCALL_STATUS_UNAVAILABLE);
	cr_assert(cap_destroy_by_id(delegated));
	cap_id_t reclaimed = acpi_test_claim_ok(provider, "TEST", 0u);
	cr_assert(cap_destroy_by_id(reclaimed));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, process_grant_cleanup_releases_claims) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[] = {0x66u};

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-process-cleanup");
	acpi_mock_add("TEST", 1u, body, sizeof(body));
	cap_id_t provider = acpi_test_provider(&ctx);
	(void)acpi_test_claim_ok(provider, "TEST", 0u);

	cap_drop_for_process(process_pid(ctx.process));
	provider = kernel_capability_acpi_grant(process_pid(ctx.process));
	cr_assert_neq(provider, CAP_ID_INVALID);
	cap_id_t reclaimed = acpi_test_claim_ok(provider, "TEST", 0u);
	cr_assert(cap_destroy_by_id(reclaimed));
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, info_and_reads_expose_only_the_table_body) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[]    = {0x10u, 0x20u, 0x30u, 0x40u, 0x50u};
	uint8_t                               output[3] = {0};

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-read");
	acpi_mock_add("TEST", 7u, body, sizeof(body));
	cap_id_t                             provider     = acpi_test_provider(&ctx);
	cap_id_t                             table        = acpi_test_claim_ok(provider, "TEST", 0u);
	struct capability*                   table_grant  = cap_acquire(table);
	const struct acpi_table_info_request info_request = {.header = {.op = ACPI_TABLE_OP_INFO}};
	struct acpi_table_info_response      info;
	cr_assert_not_null(table_grant);
	cr_assert_eq(cap_rights(table_grant), CAP_CALL | CAP_READ | CAP_DELEGATE);
	cap_release(table_grant);
	syscall_result_t result =
		kernel_capability_test_call(table, &info_request, sizeof(info_request), &info, sizeof(info));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(info));
	cr_assert_eq(info.body_size, sizeof(body));
	cr_assert_eq(info.revision, 7u);
	for (size_t index = 0u; index < sizeof(info.reserved); index++) cr_assert_eq(info.reserved[index], 0u);

	struct acpi_table_read_request read_request = {
		.header = {.op = ACPI_TABLE_OP_READ}, .offset = 1u, .size = sizeof(output)};
	result = kernel_capability_test_call(table, &read_request, sizeof(read_request), output, sizeof(output));
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, sizeof(output));
	cr_assert_eq(memcmp(output, body + 1u, sizeof(output)), 0);

	read_request.offset = sizeof(body);
	read_request.size   = 0u;
	result              = kernel_capability_test_call(table, &read_request, sizeof(read_request), NULL, 0u);
	cr_assert_eq(result.status, SYSCALL_STATUS_OK);
	cr_assert_eq(result.value, 0u);
	read_request.size = 1u;
	cr_assert_eq(kernel_capability_test_call(table, &read_request, sizeof(read_request), output, sizeof(output)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	read_request.offset = 0u;
	read_request.size   = CAP_MAX_RESPONSE_SIZE + 1u;
	cr_assert_eq(kernel_capability_test_call(table, &read_request, sizeof(read_request), output, sizeof(output)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, rights_and_malformed_requests_are_rejected) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[] = {0x77u};
	uint64_t                              count;

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-rights");
	acpi_mock_add("TEST", 1u, body, sizeof(body));
	cap_id_t           provider       = acpi_test_provider(&ctx);
	struct capability* provider_grant = cap_acquire(provider);
	cr_assert_not_null(provider_grant);
	cap_id_t read_provider = cap_delegate_create(provider_grant, process_pid(ctx.process), CAP_CALL | CAP_READ, false);
	cap_id_t manage_provider =
		cap_delegate_create(provider_grant, process_pid(ctx.process), CAP_CALL | CAP_MANAGE, false);
	cap_release(provider_grant);
	cr_assert_neq(read_provider, CAP_ID_INVALID);
	cr_assert_neq(manage_provider, CAP_ID_INVALID);
	cr_assert_eq(acpi_test_count(read_provider, "TEST", &count).status, SYSCALL_STATUS_OK);
	cr_assert_eq(count, 1u);
	struct acpi_provider_claim_response claim_response = {.table_cap = CAP_ID_INVALID};
	cr_assert_eq(acpi_test_claim(read_provider, "TEST", 0u, &claim_response).status, SYSCALL_STATUS_DENIED);
	cr_assert_eq(acpi_test_count(manage_provider, "TEST", &count).status, SYSCALL_STATUS_DENIED);

	cap_id_t           table       = acpi_test_claim_ok(provider, "TEST", 0u);
	struct capability* table_grant = cap_acquire(table);
	cr_assert_not_null(table_grant);
	cap_id_t call_only = cap_delegate_create(table_grant, process_pid(ctx.process), CAP_CALL, false);
	cap_release(table_grant);
	const struct acpi_table_info_request info_request = {.header = {.op = ACPI_TABLE_OP_INFO}};
	struct acpi_table_info_response      info;
	cr_assert_eq(
		kernel_capability_test_call(call_only, &info_request, sizeof(info_request), &info, sizeof(info)).status,
		SYSCALL_STATUS_DENIED);
	cr_assert_eq(
		kernel_capability_test_call(table, &info_request, sizeof(info_request) - 1u, &info, sizeof(info)).status,
		SYSCALL_STATUS_BAD_ARGUMENT);
	struct acpi_table_read_request read_request = {
		.header = {.op = ACPI_TABLE_OP_READ}, .reserved = 1u, .offset = 0u, .size = 0u};
	cr_assert_eq(kernel_capability_test_call(table, &read_request, sizeof(read_request), NULL, 0u).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_acpi, failed_claim_response_rolls_back_the_claim) {
	struct kernel_capability_test_context ctx;
	const uint8_t                         body[]   = {0xaau};
	struct acpi_provider_claim_response   response = {.table_cap = CAP_ID_INVALID};

	kernel_capability_test_begin(&ctx, "kernel-cap/acpi-rollback");
	acpi_mock_add("TEST", 1u, body, sizeof(body));
	cap_id_t provider = acpi_test_provider(&ctx);
	kernel_capability_acpi_test_fail_next_claim_response();
	cr_assert_eq(acpi_test_claim(provider, "TEST", 0u, &response).status, SYSCALL_STATUS_FAILED);
	cap_id_t rolled_back = kernel_capability_acpi_test_last_rollback_cap();
	cr_assert_neq(rolled_back, CAP_ID_INVALID);
	cr_assert_null(cap_acquire(rolled_back));
	cap_id_t reclaimed = acpi_test_claim_ok(provider, "TEST", 0u);
	cr_assert(cap_destroy_by_id(reclaimed));
	kernel_capability_test_end(&ctx);
}
