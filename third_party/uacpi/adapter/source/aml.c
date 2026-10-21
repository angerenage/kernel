#include <uacpi/internal/context.h>
#include <uacpi/internal/interpreter.h>
#include <uacpi/internal/log.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/internal/notify.h>
#include <uacpi/internal/opregion.h>
#include <uacpi/internal/osi.h>
#include <uacpi/internal/types.h>
#include <uacpi/kernel_api.h>
#include <uacpi/uacpi.h>
#include <uacpi_host/aml.h>

static const struct uacpi_host_aml_definition* aml_definitions;
static uacpi_size                              aml_definition_count;

static uacpi_bool bytes_are_zero(const uacpi_u8* bytes, uacpi_size size) {
	uacpi_size index;

	for (index = 0; index < size; ++index) {
		if (bytes[index] != 0) return UACPI_FALSE;
	}

	return UACPI_TRUE;
}

static uacpi_status validate_definitions(const struct uacpi_host_aml_definition* definitions, uacpi_size count) {
	uacpi_size index;

	if (definitions == UACPI_NULL || count == 0) return UACPI_STATUS_INVALID_ARGUMENT;

	for (index = 0; index < count; ++index) {
		const struct uacpi_host_aml_definition* definition = &definitions[index];
		uacpi_u8 expected_kind = index == 0 ? UACPI_HOST_AML_DEFINITION_DSDT : UACPI_HOST_AML_DEFINITION_SSDT;

		if (definition->kind != expected_kind) return UACPI_STATUS_INVALID_ARGUMENT;
		if (!bytes_are_zero(definition->reserved, sizeof(definition->reserved))) return UACPI_STATUS_INVALID_ARGUMENT;
		if (definition->aml.length > 0xFFFFFFFFu) return UACPI_STATUS_INVALID_TABLE_LENGTH;
		if (definition->aml.const_data == UACPI_NULL) return UACPI_STATUS_INVALID_ARGUMENT;
	}

	return UACPI_STATUS_OK;
}

static uacpi_status initialize_interpreter(uacpi_u64 flags) {
	uacpi_status ret;

#ifdef UACPI_KERNEL_INITIALIZATION
	ret = uacpi_kernel_initialize(UACPI_INIT_LEVEL_EARLY);
	if (uacpi_unlikely_error(ret)) return ret;
#endif

	g_uacpi_rt_ctx.init_level       = UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED;
	g_uacpi_rt_ctx.last_sleep_typ_a = UACPI_SLEEP_TYP_INVALID;
	g_uacpi_rt_ctx.last_sleep_typ_b = UACPI_SLEEP_TYP_INVALID;
	g_uacpi_rt_ctx.s0_sleep_typ_a   = UACPI_SLEEP_TYP_INVALID;
	g_uacpi_rt_ctx.s0_sleep_typ_b   = UACPI_SLEEP_TYP_INVALID;
	g_uacpi_rt_ctx.flags            = flags;
#ifndef UACPI_REDUCED_HARDWARE
	/* The sanitized interface intentionally supplies no FADT hardware data. */
	g_uacpi_rt_ctx.is_hardware_reduced = UACPI_TRUE;
#endif

	uacpi_logger_initialize();

	if (g_uacpi_rt_ctx.loop_timeout_seconds == 0) uacpi_context_set_loop_timeout(UACPI_DEFAULT_LOOP_TIMEOUT_SECONDS);
	if (g_uacpi_rt_ctx.max_call_stack_depth == 0)
		uacpi_context_set_max_call_stack_depth(UACPI_DEFAULT_MAX_CALL_STACK_DEPTH);

	ret = uacpi_initialize_opregion();
	if (uacpi_unlikely_error(ret)) return ret;

	ret = uacpi_initialize_interfaces();
	if (uacpi_unlikely_error(ret)) return ret;

	ret = uacpi_initialize_namespace();
	if (uacpi_unlikely_error(ret)) return ret;

	ret = uacpi_initialize_notify();
	if (uacpi_unlikely_error(ret)) return ret;

	uacpi_install_default_address_space_handlers();
	return UACPI_STATUS_OK;
}

uacpi_status uacpi_host_initialize(uacpi_u64 flags, const struct uacpi_host_aml_definition* definitions,
                                   uacpi_size definition_count) {
	uacpi_status ret;

	UACPI_ENSURE_INIT_LEVEL_IS(UACPI_INIT_LEVEL_EARLY);

	ret = validate_definitions(definitions, definition_count);
	if (uacpi_unlikely_error(ret)) return ret;

	aml_definitions        = definitions;
	aml_definition_count   = definition_count;
	g_uacpi_rt_ctx.is_rev1 = definitions[0].revision < 2;

	ret = initialize_interpreter(flags);
	if (uacpi_unlikely_error(ret)) {
		uacpi_host_reset();
		return ret;
	}

	return UACPI_STATUS_OK;
}

static uacpi_status execute_definition(const struct uacpi_host_aml_definition* definition) {
	uacpi_control_method method = {0};

	method.code                  = (uacpi_u8*)definition->aml.const_bytes;
	method.size                  = (uacpi_u32)definition->aml.length;
	method.named_objects_persist = UACPI_TRUE;

	uacpi_trace("load of %s OEM=%.6s TABLE=%.8s OEM_REV=%08X",
	            definition->kind == UACPI_HOST_AML_DEFINITION_DSDT ? "DSDT" : "SSDT",
	            definition->oem_id,
	            definition->oem_table_id,
	            definition->oem_revision);

	return uacpi_execute_control_method(uacpi_namespace_root(), &method, UACPI_NULL, UACPI_NULL);
}

uacpi_status uacpi_host_namespace_load(void) {
	uacpi_status ret;
	uacpi_size   index;
	uacpi_u32    failures = 0;

	UACPI_ENSURE_INIT_LEVEL_IS(UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED);

#ifdef UACPI_KERNEL_INITIALIZATION
	ret = uacpi_kernel_initialize(UACPI_INIT_LEVEL_SUBSYSTEM_INITIALIZED);
	if (uacpi_unlikely_error(ret)) goto fatal_error;
#endif

	for (index = 0; index < aml_definition_count; ++index) {
		ret = execute_definition(&aml_definitions[index]);
		if (uacpi_unlikely_error(ret)) {
			uacpi_log_lvl(index == 0 ? UACPI_LOG_ERROR : UACPI_LOG_WARN,
			              "failed to load %s at index %zu: %s",
			              index == 0 ? "DSDT" : "SSDT",
			              index,
			              uacpi_status_to_string(ret));
			failures++;
			if (index == 0) return ret;
		}
	}

	uacpi_info("loaded %zu sanitized AML blob%s (%u error%s)",
	           aml_definition_count,
	           aml_definition_count == 1 ? "" : "s",
	           failures,
	           failures == 1 ? "" : "s");

	g_uacpi_rt_ctx.init_level = UACPI_INIT_LEVEL_NAMESPACE_LOADED;
	return UACPI_STATUS_OK;

#ifdef UACPI_KERNEL_INITIALIZATION
fatal_error:
	uacpi_host_reset();
	return ret;
#endif
}

void uacpi_host_reset(void) {
	uacpi_state_reset();
	aml_definitions      = UACPI_NULL;
	aml_definition_count = 0;
}
