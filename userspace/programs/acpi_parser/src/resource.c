#include "resource.h"

#include <base/cap.h>
#include <base/interrupt.h>
#include <protocol/device.h>
#include <runtime/device.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <system/capability.h>
#include <uacpi/resources.h>

#if defined(__x86_64__)
#include <base/io_port.h>
#include <system/io_port.h>
#endif

#define ACPI_RESOURCE_NAME_CAPACITY 32u

struct acpi_resource_context {
	struct device_builder* builder;
	cap_id_t               memory_allocator_cap;
	cap_id_t               interrupts_cap;
	cap_id_t               io_ports_cap;
	size_t                 memory_count;
	size_t                 io_count;
	size_t                 interrupt_count;
	syscall_status_t       status;
};

static bool resource_name(char output[ACPI_RESOURCE_NAME_CAPACITY], const char* kind, size_t index, size_t* out_size) {
	int written = snprintf(output, ACPI_RESOURCE_NAME_CAPACITY, "%s_%zu", kind, index);
	if (written <= 0 || (size_t)written >= ACPI_RESOURCE_NAME_CAPACITY || (size_t)written > DEVICE_IDENTIFIER_MAX)
		return false;
	*out_size = (size_t)written;
	return true;
}

static syscall_status_t add_memory(struct acpi_resource_context* context, uint64_t address, uint64_t size) {
	char   name[ACPI_RESOURCE_NAME_CAPACITY];
	size_t name_size;

	if (size == 0u || address > UINTPTR_MAX || size > SIZE_MAX || address > UINTPTR_MAX - (size - 1u))
		return SYSCALL_STATUS_OK;
	if (!resource_name(name, "memory", context->memory_count, &name_size)) return SYSCALL_STATUS_FAILED;
	syscall_status_t status = device_builder_add_mmio_resource(
		context->builder, context->memory_allocator_cap, name, name_size, (uintptr_t)address, (size_t)size);
	if (status == SYSCALL_STATUS_OK) context->memory_count++;
	return status;
}

static syscall_status_t add_io(struct acpi_resource_context* context, uint64_t address, uint64_t size) {
#if defined(__x86_64__)
	static const cap_rights_t driver_rights = CAP_CALL | CAP_READ | CAP_WRITE | CAP_MAP;
	static const cap_rights_t parser_rights = driver_rights | CAP_DELEGATE;
	struct io_port_info       range;
	char                      name[ACPI_RESOURCE_NAME_CAPACITY];
	size_t                    name_size;
	cap_id_t                  cap = CAP_ID_INVALID;
	syscall_status_t          status;

	if (context->io_ports_cap == CAP_ID_INVALID || size == 0u || address > UINT32_MAX || size > UINT32_MAX ||
	    address > UINT32_MAX - (size - 1u))
		return SYSCALL_STATUS_OK;
	status = io_port_info(context->io_ports_cap, &range);
	if (status != SYSCALL_STATUS_OK) return status;
	if (address < range.base || address - range.base > range.count || size > range.count - (address - range.base))
		return SYSCALL_STATUS_OK;
	if (!resource_name(name, "io", context->io_count, &name_size)) return SYSCALL_STATUS_FAILED;
	status =
		io_port_derive(context->io_ports_cap, (uint32_t)(address - range.base), (uint32_t)size, parser_rights, &cap);
	if (status == SYSCALL_STATUS_OK)
		status = device_builder_add_resource(context->builder, name, name_size, cap, driver_rights);
	if (cap != CAP_ID_INVALID) {
		syscall_status_t drop_status = cap_drop(cap);
		if (status == SYSCALL_STATUS_OK) status = drop_status;
	}
	if (status == SYSCALL_STATUS_OK) context->io_count++;
	return status;
#else
	(void)context;
	(void)address;
	(void)size;
	return SYSCALL_STATUS_OK;
#endif
}

static syscall_status_t add_interrupt(struct acpi_resource_context* context, uint32_t source, uint8_t triggering,
                                      uint8_t polarity) {
	char                    name[ACPI_RESOURCE_NAME_CAPACITY];
	size_t                  name_size;
	enum interrupt_trigger  trigger;
	enum interrupt_polarity active;

	if (context->interrupts_cap == CAP_ID_INVALID) return SYSCALL_STATUS_OK;
	trigger = triggering == UACPI_TRIGGERING_EDGE ? INTERRUPT_TRIGGER_EDGE : INTERRUPT_TRIGGER_LEVEL;
	if (polarity == UACPI_POLARITY_ACTIVE_HIGH) active = INTERRUPT_POLARITY_HIGH;
	else if (polarity == UACPI_POLARITY_ACTIVE_LOW) active = INTERRUPT_POLARITY_LOW;
	else return SYSCALL_STATUS_OK;
	if (!resource_name(name, "interrupt", context->interrupt_count, &name_size)) return SYSCALL_STATUS_FAILED;
	syscall_status_t status = device_builder_add_interrupt_resource(context->builder,
	                                                                context->interrupts_cap,
	                                                                name,
	                                                                name_size,
	                                                                INTERRUPT_SOURCE_CONTROLLER_PLATFORM,
	                                                                source,
	                                                                trigger,
	                                                                active);
	if (status == SYSCALL_STATUS_OK) context->interrupt_count++;
	return status;
}

static syscall_status_t add_address(struct acpi_resource_context* context, const uacpi_resource_address_common* common,
                                    uint64_t minimum, uint64_t translation, uint64_t length) {
	if (common->direction != UACPI_CONSUMER || minimum > UINT64_MAX - translation) return SYSCALL_STATUS_OK;
	if (common->type == UACPI_RANGE_MEMORY) return add_memory(context, minimum + translation, length);
	if (common->type == UACPI_RANGE_IO) return add_io(context, minimum + translation, length);
	return SYSCALL_STATUS_OK;
}

static uacpi_iteration_decision add_resource(void* opaque, uacpi_resource* resource) {
	struct acpi_resource_context* context = opaque;

	switch (resource->type) {
	case UACPI_RESOURCE_TYPE_IRQ:
		if (resource->irq.sharing == UACPI_SHARED) break;
		for (uacpi_u8 index = 0u; context->status == SYSCALL_STATUS_OK && index < resource->irq.num_irqs; index++)
			context->status =
				add_interrupt(context, resource->irq.irqs[index], resource->irq.triggering, resource->irq.polarity);
		break;
	case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		if (resource->extended_irq.direction == UACPI_CONSUMER && resource->extended_irq.sharing == UACPI_EXCLUSIVE &&
		    resource->extended_irq.source.length == 0u)
			for (uacpi_u8 index = 0u; context->status == SYSCALL_STATUS_OK && index < resource->extended_irq.num_irqs;
			     index++)
				context->status = add_interrupt(context,
				                                resource->extended_irq.irqs[index],
				                                resource->extended_irq.triggering,
				                                resource->extended_irq.polarity);
		break;
	case UACPI_RESOURCE_TYPE_IO:
		context->status = add_io(context, resource->io.minimum, resource->io.length);
		break;
	case UACPI_RESOURCE_TYPE_FIXED_IO:
		context->status = add_io(context, resource->fixed_io.address, resource->fixed_io.length);
		break;
	case UACPI_RESOURCE_TYPE_MEMORY24:
		context->status = add_memory(context, resource->memory24.minimum, resource->memory24.length);
		break;
	case UACPI_RESOURCE_TYPE_MEMORY32:
		context->status = add_memory(context, resource->memory32.minimum, resource->memory32.length);
		break;
	case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		context->status = add_memory(context, resource->fixed_memory32.address, resource->fixed_memory32.length);
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS16:
		context->status = add_address(context,
		                              &resource->address16.common,
		                              resource->address16.minimum,
		                              resource->address16.translation_offset,
		                              resource->address16.address_length);
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS32:
		context->status = add_address(context,
		                              &resource->address32.common,
		                              resource->address32.minimum,
		                              resource->address32.translation_offset,
		                              resource->address32.address_length);
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS64:
		context->status = add_address(context,
		                              &resource->address64.common,
		                              resource->address64.minimum,
		                              resource->address64.translation_offset,
		                              resource->address64.address_length);
		break;
	case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
		context->status = add_address(context,
		                              &resource->address64_extended.common,
		                              resource->address64_extended.minimum,
		                              resource->address64_extended.translation_offset,
		                              resource->address64_extended.address_length);
		break;
	default:
		break;
	}
	return context->status == SYSCALL_STATUS_OK ? UACPI_ITERATION_DECISION_CONTINUE : UACPI_ITERATION_DECISION_BREAK;
}

syscall_status_t acpi_resources_add_current(struct acpi_device* device, uacpi_namespace_node* node,
                                            cap_id_t memory_allocator_cap, cap_id_t interrupts_cap,
                                            cap_id_t io_ports_cap) {
	struct acpi_resource_context context;
	uacpi_status                 status;

	if (device == NULL || node == NULL || memory_allocator_cap == CAP_ID_INVALID) return SYSCALL_STATUS_BAD_ARGUMENT;
	context = (struct acpi_resource_context){
		.builder              = acpi_device_builder(device),
		.memory_allocator_cap = memory_allocator_cap,
		.interrupts_cap       = interrupts_cap,
		.io_ports_cap         = io_ports_cap,
		.status               = SYSCALL_STATUS_OK,
	};
	status = uacpi_for_each_device_resource(node, "_CRS", add_resource, &context);
	if (status == UACPI_STATUS_NOT_FOUND) return SYSCALL_STATUS_OK;
	if (status != UACPI_STATUS_OK) {
		printf("acpi-parser: ignoring unusable _CRS: %s\n", uacpi_status_to_string(status));
		return SYSCALL_STATUS_OK;
	}
	if (context.status != SYSCALL_STATUS_OK)
		printf("acpi-parser: unable to publish an _CRS resource: %u\n", (unsigned)context.status);
	return SYSCALL_STATUS_OK;
}
