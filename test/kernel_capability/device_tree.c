#include "../../kernel/src/capability/device_tree.h"

#include <base/device_tree.h>
#include <base/syscall.h>
#include <core/capability.h>
#include <criterion/criterion.h>
#include <firmware/dt.h>
#include <hal/device_tree.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_support.h"

enum {
	DT_TEST_ROOT         = 10u,
	DT_TEST_VISIBLE      = 20u,
	DT_TEST_INTERRUPT    = 30u,
	DT_TEST_HIDDEN_CHILD = 31u,
	DT_TEST_AMBIGUOUS_A  = 32u,
	DT_TEST_AMBIGUOUS_B  = 33u,
	DT_TEST_AFTER        = 40u,
	DT_TEST_DMA          = 50u,
	DT_TEST_UNSUPPORTED  = 60u,
};

struct dt_test_node {
	size_t      id;
	size_t      parent;
	size_t      child;
	size_t      next;
	const char* name;
	uint32_t    phandle;
};

static const struct dt_test_node dt_test_nodes[] = {
	{        DT_TEST_ROOT,          SIZE_MAX,      DT_TEST_VISIBLE,            SIZE_MAX,               "", 0u},
	{     DT_TEST_VISIBLE,      DT_TEST_ROOT,             SIZE_MAX,   DT_TEST_INTERRUPT,      "visible@0", 1u},
	{   DT_TEST_INTERRUPT,      DT_TEST_ROOT, DT_TEST_HIDDEN_CHILD,       DT_TEST_AFTER, "interrupt@1000", 2u},
	{DT_TEST_HIDDEN_CHILD, DT_TEST_INTERRUPT,             SIZE_MAX, DT_TEST_AMBIGUOUS_A,   "hidden-child", 6u},
	{ DT_TEST_AMBIGUOUS_A, DT_TEST_INTERRUPT,             SIZE_MAX, DT_TEST_AMBIGUOUS_B,    "ambiguous-a", 7u},
	{ DT_TEST_AMBIGUOUS_B, DT_TEST_INTERRUPT,             SIZE_MAX,            SIZE_MAX,    "ambiguous-b", 7u},
	{       DT_TEST_AFTER,      DT_TEST_ROOT,             SIZE_MAX,         DT_TEST_DMA,          "after", 3u},
	{         DT_TEST_DMA,      DT_TEST_ROOT,             SIZE_MAX, DT_TEST_UNSUPPORTED,     "iommu@2000", 4u},
	{ DT_TEST_UNSUPPORTED,      DT_TEST_ROOT,             SIZE_MAX,            SIZE_MAX,          "owned", 5u},
};

static const uint8_t  dt_test_raw[] = {0xaau, 0xbbu, 0xccu};
static bool           dt_test_available;
static bool           dt_test_conflict;
static struct dt_node dt_test_invalid_report = DT_NODE_INVALID;
static size_t         dt_test_report_upper_calls;
static size_t         dt_test_report_fill_calls;
static size_t         dt_test_report_fill_capacity;

void dt_mock_set_available(bool available) {
	dt_test_available = available;
}

static const struct dt_test_node* dt_test_node_find(size_t id) {
	for (size_t index = 0u; index < sizeof(dt_test_nodes) / sizeof(dt_test_nodes[0]); index++)
		if (dt_test_nodes[index].id == id) return &dt_test_nodes[index];
	return NULL;
}

struct dt_node dt_root(void) {
	return dt_test_available ? (struct dt_node){.id = DT_TEST_ROOT} : DT_NODE_INVALID;
}

bool dt_node_valid(struct dt_node node) {
	return dt_test_available && dt_test_node_find(node.id) != NULL;
}

struct dt_node dt_node_child(struct dt_node node) {
	const struct dt_test_node* found = dt_test_node_find(node.id);
	return found != NULL && found->child != SIZE_MAX ? (struct dt_node){.id = found->child} : DT_NODE_INVALID;
}

struct dt_node dt_node_next(struct dt_node node) {
	const struct dt_test_node* found = dt_test_node_find(node.id);
	return found != NULL && found->next != SIZE_MAX ? (struct dt_node){.id = found->next} : DT_NODE_INVALID;
}

struct dt_node dt_node_parent(struct dt_node node) {
	const struct dt_test_node* found = dt_test_node_find(node.id);
	return found != NULL && found->parent != SIZE_MAX ? (struct dt_node){.id = found->parent} : DT_NODE_INVALID;
}

const char* dt_node_name(struct dt_node node) {
	const struct dt_test_node* found = dt_test_node_find(node.id);
	return found == NULL ? NULL : found->name;
}

bool dt_node_property_at(struct dt_node node, size_t index, const char** out_name, struct dt_property* out) {
	const struct dt_test_node* found = dt_test_node_find(node.id);
	static uint8_t             phandle[4];

	if (found == NULL || out_name == NULL || out == NULL || found->phandle == 0u) return false;
	if (index == 0u) {
		phandle[0] = (uint8_t)(found->phandle >> 24u);
		phandle[1] = (uint8_t)(found->phandle >> 16u);
		phandle[2] = (uint8_t)(found->phandle >> 8u);
		phandle[3] = (uint8_t)found->phandle;
		*out_name  = "phandle";
		*out       = (struct dt_property){.data = phandle, .size = sizeof(phandle)};
		return true;
	}
	if (node.id == DT_TEST_VISIBLE && index == 1u) {
		*out_name = "raw";
		*out      = (struct dt_property){.data = dt_test_raw, .size = sizeof(dt_test_raw)};
		return true;
	}
	return false;
}

struct dt_node dt_node_by_phandle(uint32_t phandle) {
	struct dt_node found = DT_NODE_INVALID;

	for (size_t index = 0u; index < sizeof(dt_test_nodes) / sizeof(dt_test_nodes[0]); index++)
		if (dt_test_available && dt_test_nodes[index].phandle == phandle) {
			if (dt_node_valid(found)) return DT_NODE_INVALID;
			found = (struct dt_node){.id = dt_test_nodes[index].id};
		}
	return found;
}

size_t hal_device_tree_consumed_nodes(struct hal_device_tree_consumed_node* nodes, size_t capacity) {
	struct hal_device_tree_consumed_node records[6] = {
		{  .node = {.id = DT_TEST_INTERRUPT},          .kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED,      .value = 0u},
		{  .node = {.id = DT_TEST_INTERRUPT}, .kind = HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER, .value = 0x1000u},
		{        .node = {.id = DT_TEST_DMA},       .kind = HAL_DEVICE_TREE_REFERENCE_DMA_CONTROLLER, .value = 0x2000u},
		{.node = {.id = DT_TEST_UNSUPPORTED},          .kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED,      .value = 0u},
		{  .node = {.id = DT_TEST_INTERRUPT}, .kind = HAL_DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER, .value = 0x3000u},
	};
	size_t count = dt_test_conflict ? 5u : 4u;
	if (dt_test_invalid_report.id != SIZE_MAX) {
		records[count++] = (struct hal_device_tree_consumed_node){
			.node = dt_test_invalid_report, .kind = HAL_DEVICE_TREE_REFERENCE_UNSUPPORTED, .value = 0u};
	}
	size_t copy_count = count < capacity ? count : capacity;

	if (nodes == NULL) dt_test_report_upper_calls++;
	else {
		dt_test_report_fill_calls++;
		dt_test_report_fill_capacity = capacity;
		memcpy(nodes, records, copy_count * sizeof(*nodes));
	}
	return count;
}

bool hal_device_tree_interrupt_translate(const struct hal_device_tree_consumed_node* controller, const uint32_t* cells,
                                         size_t cell_count, uint32_t* out_local_source_id,
                                         enum hal_interrupt_trigger*  out_trigger,
                                         enum hal_interrupt_polarity* out_polarity) {
	(void)controller;
	(void)cells;
	(void)cell_count;
	(void)out_local_source_id;
	(void)out_trigger;
	(void)out_polarity;
	return false;
}

static cap_id_t dt_test_provider(struct kernel_capability_test_context* ctx) {
	dt_mock_set_available(true);
	cr_assert(kernel_capability_device_tree_init());
	cr_assert_eq(dt_test_report_upper_calls, 1u);
	cr_assert_eq(dt_test_report_fill_calls, 1u);
	cr_assert_eq(dt_test_report_fill_capacity, 4u);
	cap_id_t provider = kernel_capability_device_tree_grant(process_pid(ctx->process));
	cr_assert_neq(provider, CAP_ID_INVALID);
	return provider;
}

static syscall_result_t dt_test_call(cap_id_t provider, const void* request, size_t request_size, void* response,
                                     size_t response_size) {
	return kernel_capability_test_call(provider, request, request_size, response, response_size);
}

Test(kernel_capability_device_tree, navigation_reads_and_phandles_use_the_filtered_snapshot) {
	struct kernel_capability_test_context ctx;
	struct device_tree_root_response      root;
	struct device_tree_node_info_response root_info;
	struct device_tree_node_info_response visible_info;
	struct device_tree_node_info_response after_info;
	const struct device_tree_root_request root_request = {.header = {.op = DEVICE_TREE_OP_ROOT}};

	kernel_capability_test_begin(&ctx, "kernel-cap/device-tree");
	cap_id_t provider = dt_test_provider(&ctx);
	cr_assert_eq(dt_test_call(provider, &root_request, sizeof(root_request), &root, sizeof(root)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(root.root, 1u);
	struct device_tree_node_info_request info_request = {
		.header = {.op = DEVICE_TREE_OP_NODE_INFO}, .reserved = 0u, .node = root.root};
	cr_assert_eq(dt_test_call(provider, &info_request, sizeof(info_request), &root_info, sizeof(root_info)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(root_info.parent, DEVICE_TREE_NODE_INVALID);
	cr_assert_eq(root_info.first_child, 2u);
	cr_assert_eq(root_info.name_size, 0u);
	info_request.node = root_info.first_child;
	cr_assert_eq(
		dt_test_call(provider, &info_request, sizeof(info_request), &visible_info, sizeof(visible_info)).status,
		SYSCALL_STATUS_OK);
	cr_assert_eq(visible_info.parent, root.root);
	cr_assert_eq(visible_info.first_child, DEVICE_TREE_NODE_INVALID);
	cr_assert_eq(visible_info.next_sibling, 3u);
	cr_assert_eq(visible_info.name_size, strlen("visible@0"));
	cr_assert_eq(visible_info.property_count, 2u);
	info_request.node = visible_info.next_sibling;
	cr_assert_eq(dt_test_call(provider, &info_request, sizeof(info_request), &after_info, sizeof(after_info)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(after_info.next_sibling, DEVICE_TREE_NODE_INVALID);

	struct device_tree_property_info_request property_request = {
		.header         = {.op = DEVICE_TREE_OP_PROPERTY_INFO},
		.reserved       = 0u,
		.node           = root_info.first_child,
		.property_index = 1u,
	};
	struct device_tree_property_info_response property_info;
	cr_assert_eq(
		dt_test_call(provider, &property_request, sizeof(property_request), &property_info, sizeof(property_info))
			.status,
		SYSCALL_STATUS_OK);
	cr_assert_eq(property_info.name_size, 3u);
	cr_assert_eq(property_info.value_size, sizeof(dt_test_raw));
	struct device_tree_read_request read_request = {
		.header         = {.op = DEVICE_TREE_OP_READ},
		.kind           = DEVICE_TREE_READ_NODE_NAME,
		.node           = root_info.first_child,
		.property_index = 0u,
		.offset         = 0u,
		.size           = strlen("visible@0"),
	};
	char name[sizeof("visible@0") - 1u];
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), name, sizeof(name)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_arr_eq(name, "visible@0", sizeof(name));
	read_request.kind           = DEVICE_TREE_READ_PROPERTY_NAME;
	read_request.property_index = 1u;
	read_request.size           = strlen("raw");
	char property_name[sizeof("raw") - 1u];
	cr_assert_eq(
		dt_test_call(provider, &read_request, sizeof(read_request), property_name, sizeof(property_name)).status,
		SYSCALL_STATUS_OK);
	cr_assert_arr_eq(property_name, "raw", sizeof(property_name));
	read_request.kind                       = DEVICE_TREE_READ_PROPERTY_VALUE;
	read_request.size                       = sizeof(dt_test_raw);
	uint8_t full_value[sizeof(dt_test_raw)] = {0};
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), full_value, sizeof(full_value)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_arr_eq(full_value, dt_test_raw, sizeof(full_value));
	read_request.offset = sizeof(dt_test_raw);
	read_request.size   = 0u;
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), NULL, 0u).status, SYSCALL_STATUS_OK);

	read_request.offset = 1u;
	read_request.size   = 2u;
	uint8_t bytes[2]    = {0};
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), bytes, sizeof(bytes)).status,
	             SYSCALL_STATUS_OK);
	cr_assert_eq(bytes[0], 0xbbu);
	cr_assert_eq(bytes[1], 0xccu);

	const enum device_tree_reference_kind kinds[] = {
		DEVICE_TREE_REFERENCE_NODE,
		DEVICE_TREE_REFERENCE_INTERRUPT_CONTROLLER,
		DEVICE_TREE_REFERENCE_NODE,
		DEVICE_TREE_REFERENCE_DMA_CONTROLLER,
		DEVICE_TREE_REFERENCE_UNSUPPORTED,
		DEVICE_TREE_REFERENCE_UNSUPPORTED,
	};
	const uint64_t values[] = {2u, 0x1000u, 3u, 0x2000u, 0u, 0u};
	for (uint32_t phandle = 1u; phandle <= 6u; phandle++) {
		struct device_tree_resolve_phandle_request  resolve_request = {.header = {.op = DEVICE_TREE_OP_RESOLVE_PHANDLE},
		                                                               .phandle = phandle};
		struct device_tree_resolve_phandle_response response;
		cr_assert_eq(
			dt_test_call(provider, &resolve_request, sizeof(resolve_request), &response, sizeof(response)).status,
			SYSCALL_STATUS_OK);
		cr_assert_eq(response.kind, kinds[phandle - 1u]);
		cr_assert_eq(response.value, values[phandle - 1u]);
	}
	struct device_tree_resolve_phandle_request  missing_request = {.header  = {.op = DEVICE_TREE_OP_RESOLVE_PHANDLE},
	                                                               .phandle = 99u};
	struct device_tree_resolve_phandle_response missing_response;
	cr_assert_eq(
		dt_test_call(provider, &missing_request, sizeof(missing_request), &missing_response, sizeof(missing_response))
			.status,
		SYSCALL_STATUS_UNAVAILABLE);
	missing_request.phandle = 7u;
	cr_assert_eq(
		dt_test_call(provider, &missing_request, sizeof(missing_request), &missing_response, sizeof(missing_response))
			.status,
		SYSCALL_STATUS_UNAVAILABLE);
	info_request.node = 4u;
	cr_assert_eq(dt_test_call(provider, &info_request, sizeof(info_request), &after_info, sizeof(after_info)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device_tree, rejects_conflicting_consumer_reports) {
	struct kernel_capability_test_context ctx;

	kernel_capability_test_begin(&ctx, "kernel-cap/device-tree-conflict");
	dt_mock_set_available(true);
	dt_test_conflict = true;
	cr_assert_not(kernel_capability_device_tree_init());
	cr_assert_not(kernel_capability_device_tree_available());
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device_tree, rejects_invalid_and_root_consumer_reports) {
	struct kernel_capability_test_context ctx;

	kernel_capability_test_begin(&ctx, "kernel-cap/device-tree-invalid-report");
	dt_mock_set_available(true);
	dt_test_invalid_report = (struct dt_node){.id = 999u};
	cr_assert_not(kernel_capability_device_tree_init());
	dt_test_invalid_report = dt_root();
	cr_assert_not(kernel_capability_device_tree_init());
	cr_assert_not(kernel_capability_device_tree_available());
	kernel_capability_test_end(&ctx);
}

Test(kernel_capability_device_tree, enforces_rights_and_request_ranges) {
	struct kernel_capability_test_context ctx;

	kernel_capability_test_begin(&ctx, "kernel-cap/device-tree-errors");
	cap_id_t           provider = dt_test_provider(&ctx);
	struct capability* grant    = cap_acquire(provider);
	cr_assert_not_null(grant);
	cap_id_t call_only = cap_delegate_create(grant, process_pid(ctx.process), CAP_CALL, false);
	cap_release(grant);
	cr_assert_neq(call_only, CAP_ID_INVALID);
	const struct device_tree_root_request root_request = {.header = {.op = DEVICE_TREE_OP_ROOT}};
	struct device_tree_root_response      root;
	cr_assert_eq(dt_test_call(call_only, &root_request, sizeof(root_request), &root, sizeof(root)).status,
	             SYSCALL_STATUS_DENIED);
	cr_assert_eq(dt_test_call(provider, &root_request, sizeof(root_request) - 1u, &root, sizeof(root)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	cr_assert_eq(dt_test_call(provider, &root_request, sizeof(root_request), &root, sizeof(root) - 1u).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	struct device_tree_node_info_request info_request = {
		.header = {.op = DEVICE_TREE_OP_NODE_INFO}, .reserved = 1u, .node = 1u};
	struct device_tree_node_info_response info_response;
	cr_assert_eq(
		dt_test_call(provider, &info_request, sizeof(info_request), &info_response, sizeof(info_response)).status,
		SYSCALL_STATUS_BAD_ARGUMENT);
	struct device_tree_read_request read_request = {
		.header         = {.op = DEVICE_TREE_OP_READ},
		.kind           = DEVICE_TREE_READ_NODE_NAME,
		.node           = 2u,
		.property_index = 0u,
		.offset         = strlen("visible@0"),
		.size           = 0u,
	};
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), NULL, 0u).status, SYSCALL_STATUS_OK);
	read_request.size = 1u;
	uint8_t byte;
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), &byte, sizeof(byte)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	read_request.offset = 0u;
	read_request.size   = CAP_MAX_RESPONSE_SIZE + 1u;
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), &byte, sizeof(byte)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	read_request.size = 0u;
	read_request.kind = (enum device_tree_read_kind)UINT32_MAX;
	cr_assert_eq(dt_test_call(provider, &read_request, sizeof(read_request), NULL, 0u).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	const struct device_tree_resolve_phandle_request zero_phandle = {.header  = {.op = DEVICE_TREE_OP_RESOLVE_PHANDLE},
	                                                                 .phandle = 0u};
	struct device_tree_resolve_phandle_response      reference;
	cr_assert_eq(dt_test_call(provider, &zero_phandle, sizeof(zero_phandle), &reference, sizeof(reference)).status,
	             SYSCALL_STATUS_BAD_ARGUMENT);
	kernel_capability_test_end(&ctx);
}
