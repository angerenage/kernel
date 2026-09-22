#include <core/cpu.h>
#include <core/sched.h>
#include <core/spinlock.h>
#include <hal/cache.h>
#include <hal/interrupts.h>
#include <kernel/cpu_boot.h>
#include <string.h>

#include "../selftest.h"
#include "sync_helpers.h"

#if defined(PLATFORM_PC_RISCV64) || defined(PLATFORM_PC_LOONGARCH64)
#include "../../../platforms/iommu_fdt.h"
#endif

#if defined(PLATFORM_PC_RISCV64)
#include "../../../platforms/pc_riscv64/hal/interrupts/imsic.h"
#endif

#if defined(PLATFORM_PC_LOONGARCH64)
#include "../../../platforms/pc_loongarch64/hal/interrupts/controller.h"
#endif

#if defined(PLATFORM_PC_X86_64)
static void kernel_selftest_cpu_x86_interrupt_ranges_are_stable(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_message_range low;
	struct hal_interrupt_message_range high;
	struct hal_interrupt_source        source = {.domain = 0u, .number = 0u};
	struct hal_interrupt_source_info   source_before;
	struct hal_interrupt_source_info   source_after;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_count() == 2u);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_at(0u, &low));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_at(1u, &high));
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_range_at(2u, &high));
	KERNEL_SELFTEST_ASSERT(ctx, low.domain == high.domain && low.delivery.domain == high.delivery.domain);
	KERNEL_SELFTEST_ASSERT(ctx, low.delivery.base == 48u && low.delivery.limit == 0x80u);
	KERNEL_SELFTEST_ASSERT(ctx, high.delivery.base == 0x81u && high.delivery.limit == 0xfeu);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&source, &source_before));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_target_supported(&source, cpu_current()));
	struct hal_interrupt_delivery source_delivery = {
		.target   = cpu_current(),
		.event    = {.domain = source_before.delivery.domain, .id = source_before.delivery.base},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
    };
	struct hal_interrupt_source_state source_state = {0};
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_init(&source_state, &source, &source_delivery));
	KERNEL_SELFTEST_ASSERT(ctx, source_state.initialized && source_state.masked);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_mask(&source_state));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_deinit(&source_state));
	KERNEL_SELFTEST_ASSERT(ctx, !source_state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&source, &source_after));
	KERNEL_SELFTEST_ASSERT(ctx,
	                       source_before.delivery.domain == source_after.delivery.domain &&
	                           source_before.delivery.base == source_after.delivery.base &&
	                           source_before.delivery.limit == source_after.delivery.limit &&
	                           source_before.target_kind == source_after.target_kind &&
	                           source_before.fixed_target == source_after.fixed_target);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_count() == 2u);
	struct hal_interrupt_message         message;
	struct hal_interrupt_message_state   state   = {0};
	struct hal_interrupt_message_request request = {
		.domain = low.domain,
		.target = cpu_current(),
		.event  = {.domain = low.delivery.domain, .id = low.delivery.limit - 1u}
    };
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_target_supported(request.domain, request.source, request.target));
	struct cpu unsupported = {.arch_id = 256u};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_target_supported(request.domain, request.source, &unsupported));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
	request.event.id = 0x80u;
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_init(&state, &request, &message));
	request.event.id = high.delivery.base;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
}
#endif

#if defined(PLATFORM_PC_AARCH64)
static void kernel_selftest_cpu_gic_external_source_contract(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_source_domain_info domain;
	struct hal_interrupt_source_info        info;
	struct hal_interrupt_source             timer = {.domain = 0u, .number = 27u};
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_count() == 1u);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_at(0u, &domain));
	KERNEL_SELFTEST_ASSERT(ctx, domain.domain == 0u && domain.first_source == 32u && domain.source_count > 0u);
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&timer, &info));
	struct hal_interrupt_source spi = {.domain = domain.domain, .number = domain.first_source};
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&spi, &info));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_target_supported(&spi, cpu_current()));
	struct cpu unsupported = {.index = SIZE_MAX};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_target_supported(&spi, &unsupported));
	KERNEL_SELFTEST_ASSERT(ctx,
	                       info.target_kind == HAL_INTERRUPT_TARGET_ROUTABLE && info.fixed_target == NULL &&
	                           info.delivery.domain == 0u && info.delivery.base == spi.number &&
	                           info.delivery.limit == spi.number + 1u);
	struct hal_interrupt_source invalid = {.domain = domain.domain,
	                                       .number = domain.first_source + domain.source_count};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&invalid, &info));
}
#endif

#define KERNEL_SELFTEST_CPU_MAX_CPUS 64u
#define KERNEL_SELFTEST_CPU_REMOTE_DISPATCH_TIMEOUT_MS 250u

static void kernel_selftest_cpu_interrupt_source_lifecycle(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_source             source = {0};
	struct hal_interrupt_source_info        info;
	struct hal_interrupt_source_domain_info domain;
	bool                                    found = false;
	for (size_t domain_index = 0u; domain_index < hal_interrupt_source_domain_count() && !found; domain_index++) {
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_at(domain_index, &domain));
		for (uint32_t offset = 0u; offset < domain.source_count; offset++) {
			source = (struct hal_interrupt_source){.domain = domain.domain, .number = domain.first_source + offset};
			if (hal_interrupt_source_info(&source, &info)) {
				found = true;
				break;
			}
		}
	}
	if (!found) return;
	const struct cpu* target = info.target_kind == HAL_INTERRUPT_TARGET_FIXED ? info.fixed_target : cpu_current();
	KERNEL_SELFTEST_ASSERT(ctx, target != NULL && hal_interrupt_source_target_supported(&source, target));
	struct hal_interrupt_delivery delivery = {
		.target   = target,
		.event    = {.domain = info.delivery.domain, .id = info.delivery.limit},
		.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
		.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
    };
	struct hal_interrupt_source_state state = {0};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_init(&state, &source, &delivery));
	KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
	delivery.event.id = info.delivery.base;
	bool initialized  = hal_interrupt_source_init(&state, &source, &delivery);
	if (!initialized) {
		KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
		delivery.trigger  = HAL_INTERRUPT_TRIGGER_EDGE;
		delivery.polarity = HAL_INTERRUPT_POLARITY_HIGH;
		initialized       = hal_interrupt_source_init(&state, &source, &delivery);
	}
	KERNEL_SELFTEST_ASSERT(ctx, initialized && state.initialized && state.masked);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_mask(&state) && state.masked);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_deinit(&state) && !state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_deinit(&state));
}

static void kernel_selftest_cpu_interrupt_message_lifecycle(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_message_range range;
	if (hal_interrupt_message_range_count() == 0u) return;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_at(0u, &range));
	struct hal_interrupt_message_request request = {
		.domain = range.domain,
		.target = cpu_current(),
		.event  = {.domain = range.delivery.domain, .id = range.delivery.limit}
    };
	if (!hal_interrupt_message_target_supported(request.domain, request.source, request.target)) return;
	struct hal_interrupt_message       message;
	struct hal_interrupt_message_state state = {0};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
	request.event.id = range.delivery.base;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx, state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_init(&state, &request, &message) && state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state) && !state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
}

static void kernel_selftest_cpu_global_executable_sync(struct kernel_selftest_context* ctx) {
	hal_cache_sync_executable_range_all_cpus((void*)(uintptr_t)&kernel_selftest_cpu_global_executable_sync, 1u);
	KERNEL_SELFTEST_ASSERT(ctx, true);
}

#if defined(PLATFORM_PC_RISCV64)
static void kernel_selftest_cpu_plic_firmware_source_contract(struct kernel_selftest_context* ctx) {
	uintptr_t                        controller   = 0u;
	struct hal_interrupt_source      source       = {.domain = 1u, .number = 1u};
	struct hal_interrupt_source      aplic_source = {.domain = 2u, .number = 1u};
	struct hal_interrupt_source_info info;
	size_t                           described = iommu_fdt_controllers("sifive,plic-1.0.0", 0u, &controller);
	if (described == 0u) described = iommu_fdt_controllers("riscv,plic0", 0u, &controller);
	bool available       = hal_interrupt_source_info(&source, &info);
	bool aplic_available = hal_interrupt_source_info(&aplic_source, &info);
	if (described == 0u) {
		KERNEL_SELFTEST_ASSERT(ctx, !available);
		struct hal_interrupt_source aggregate = {.domain = 0u, .number = 9u};
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_count() == (aplic_available ? 1u : 0u));
		if (aplic_available) {
			struct hal_interrupt_source_domain_info domain;
			KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_at(0u, &domain));
			KERNEL_SELFTEST_ASSERT(ctx, domain.domain == 2u && domain.first_source == 1u);
		}
		KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&aggregate, &info));
		return;
	}
	KERNEL_SELFTEST_ASSERT(
		ctx, iommu_fdt_compatible_present("sifive,plic-1.0.0") || iommu_fdt_compatible_present("riscv,plic0"));
	if (described != 1u || controller == 0u || !available) {
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_count() == (aplic_available ? 1u : 0u));
		return;
	}
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&source, &info));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_target_supported(&source, cpu_current()));
	struct cpu unsupported = {.arch_id = UINT64_MAX};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_target_supported(&source, &unsupported));
	KERNEL_SELFTEST_ASSERT(ctx,
	                       info.target_kind == HAL_INTERRUPT_TARGET_ROUTABLE && info.fixed_target == NULL &&
	                           info.delivery.base == source.number && info.delivery.limit == source.number + 1u);
	struct hal_interrupt_source_domain_info domain;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_count() == (aplic_available ? 2u : 1u));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_at(0u, &domain));
	KERNEL_SELFTEST_ASSERT(ctx, domain.domain == 1u && domain.first_source == 1u && domain.source_count > 0u);
	struct hal_interrupt_source aggregate = {.domain = 0u, .number = 9u};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&aggregate, &info));
	source.number = domain.first_source + domain.source_count;
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&source, &info));
}

static void kernel_selftest_cpu_imsic_message_contract(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_message_range range;
	bool                               described = iommu_fdt_compatible_present("riscv,imsics");
	if (!described) {
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_count() == 0u);
		KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_range_at(0u, &range));
		return;
	}
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_count() == 1u);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_at(0u, &range));
	KERNEL_SELFTEST_ASSERT(ctx, range.delivery.base == 1u && range.delivery.limit > range.delivery.base);
	size_t         selected_controller;
	const uint8_t* ids_property;
	size_t         ids_property_size;
	KERNEL_SELFTEST_ASSERT(
		ctx,
		riscv64_imsic_selected_controller(&selected_controller) &&
			iommu_fdt_controller_property(
				"riscv,imsics", selected_controller, "riscv,num-ids", &ids_property, &ids_property_size) &&
			ids_property_size == 4u && range.delivery.limit == iommu_fdt_u32(ids_property) + 1u);
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_message_range_at(1u, &range));
	struct hal_interrupt_message_request request = {
		.domain = range.domain,
		.target = cpu_current(),
		.event  = {.domain = range.delivery.domain, .id = range.delivery.limit - 1u}
    };
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_target_supported(request.domain, request.source, request.target));
	struct hal_interrupt_message       message;
	struct hal_interrupt_message_state state = {0};
	KERNEL_SELFTEST_ASSERT(
		ctx,
		!hal_interrupt_message_init(
			&state,
			&(struct hal_interrupt_message_request){
				.domain = range.domain, .target = cpu_current(), .event = {.domain = range.delivery.domain, .id = 0u}
    },
			&message));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx, message.address != 0u && message.address % 4096u == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, message.data == request.event.id);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
	if (cpu_count() > 1u) {
		const struct cpu* remote = cpu_by_index((cpu_current()->index + 1u) % cpu_count());
		if (remote != NULL && cpu_state_get(remote) == CPU_STATE_ONLINE) {
			uint64_t local_address = message.address;
			request.target         = remote;
			request.event.id       = range.delivery.limit - 2u;
			KERNEL_SELFTEST_ASSERT(
				ctx, hal_interrupt_message_target_supported(request.domain, request.source, request.target));
			KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
			KERNEL_SELFTEST_ASSERT(ctx, message.address != local_address);
			KERNEL_SELFTEST_ASSERT(ctx, message.data == request.event.id);
			KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
		}
	}
}

static void kernel_selftest_cpu_aplic_source_starts_masked(struct kernel_selftest_context* ctx) {
	if (!iommu_fdt_compatible_present("qemu,aplic")) return;
	struct hal_interrupt_source_domain_info domain;
	if (!hal_interrupt_source_domain_at(0u, &domain) || domain.domain != 2u) return;
	struct hal_interrupt_source      source = {.domain = domain.domain, .number = domain.source_count};
	struct hal_interrupt_source_info info;
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&source, &info));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_target_supported(&source, cpu_current()));
	struct hal_interrupt_delivery delivery = {
		.target   = cpu_current(),
		.event    = {.domain = info.delivery.domain, .id = info.delivery.base},
		.trigger  = HAL_INTERRUPT_TRIGGER_EDGE,
		.polarity = HAL_INTERRUPT_POLARITY_HIGH
    };
	struct hal_interrupt_source_state state   = {0};
	struct hal_interrupt_delivery     invalid = delivery;
	invalid.event.id                          = info.delivery.limit;
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_init(&state, &source, &invalid));
	KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_init(&state, &source, &delivery));
	KERNEL_SELFTEST_ASSERT(ctx, state.initialized && state.masked);
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_mask(&state));
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_deinit(&state));
	KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
}
#endif

#if defined(PLATFORM_PC_LOONGARCH64)
static void kernel_selftest_cpu_loongarch_interrupt_contract(struct kernel_selftest_context* ctx) {
	struct hal_interrupt_source_info info;
	struct hal_interrupt_source      aggregate = {.domain = 0u, .number = 3u};
	KERNEL_SELFTEST_ASSERT(ctx, !hal_interrupt_source_info(&aggregate, &info));

	bool                                    pch_described = iommu_fdt_compatible_present("loongson,pch-pic-1.0");
	struct hal_interrupt_source_domain_info pch_domain    = {0};
	bool                                    found_pch     = false;
	for (size_t index = 0u; index < hal_interrupt_source_domain_count(); index++) {
		struct hal_interrupt_source_domain_info domain;
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_domain_at(index, &domain));
		if (domain.domain >= 16u && domain.domain < 20u) {
			pch_domain = domain;
			found_pch  = true;
		}
	}
	if (pch_described) KERNEL_SELFTEST_ASSERT(ctx, found_pch);
	if (found_pch) {
		struct hal_interrupt_source source = {.domain = pch_domain.domain,
		                                      .number = pch_domain.first_source + pch_domain.source_count - 1u};
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_info(&source, &info));
		struct hal_interrupt_delivery delivery = {
			.target   = cpu_current(),
			.event    = {.domain = info.delivery.domain, .id = info.delivery.base},
			.trigger  = HAL_INTERRUPT_TRIGGER_FIRMWARE,
			.polarity = HAL_INTERRUPT_POLARITY_FIRMWARE
        };
		struct hal_interrupt_source_state state = {0};
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_init(&state, &source, &delivery));
		KERNEL_SELFTEST_ASSERT(ctx, state.initialized && state.masked);
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_mask(&state));
		KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_source_deinit(&state));
		KERNEL_SELFTEST_ASSERT(ctx, !state.initialized);
	}

	struct hal_interrupt_message_range message_range;
	bool                               msi_described = iommu_fdt_compatible_present("loongson,pch-msi-1.0");
	if (!hal_interrupt_message_range_at(0u, &message_range)) {
		KERNEL_SELFTEST_ASSERT(ctx, !msi_described && hal_interrupt_message_range_count() == 0u);
		return;
	}
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_range_count() == 1u);
	struct hal_interrupt_message_request request = {
		.domain = message_range.domain,
		.target = cpu_current(),
		.event  = {.domain = message_range.delivery.domain, .id = message_range.delivery.limit - 1u}
    };
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_target_supported(request.domain, request.source, request.target));
	struct hal_interrupt_message       message;
	struct hal_interrupt_message_state state = {0};
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_init(&state, &request, &message));
	KERNEL_SELFTEST_ASSERT(ctx,
	                       message.address != 0u && state.initialized && state.event.domain == request.event.domain &&
	                           state.event.id == request.event.id);
	if (request.domain == LOONGARCH64_MESSAGE_DOMAIN_AVEC) {
		uint64_t expected = (pch_msi.address - LOONGARCH64_AVEC_MESSAGE_OFFSET) | ((uint64_t)request.event.id << 4u) |
		                    (request.target->arch_id << 12u);
		KERNEL_SELFTEST_ASSERT(ctx, message.address == expected && message.data == 0u);
	}
	else if (request.domain == LOONGARCH64_MESSAGE_DOMAIN_REDIRECT) {
		uint64_t expected = (pch_msi.address - LOONGARCH64_AVEC_MESSAGE_OFFSET) | (1u << 2u);
		KERNEL_SELFTEST_ASSERT(
			ctx, message.address == expected && message.data == state.redirect_index && message.data < 65536u);
	}
	else {
		KERNEL_SELFTEST_ASSERT(ctx,
		                       request.domain == LOONGARCH64_MESSAGE_DOMAIN_PCH_MSI &&
		                           message.address == pch_msi.address && message.data == request.event.id);
	}
	KERNEL_SELFTEST_ASSERT(ctx, hal_interrupt_message_deinit(&state));
}
#endif

static void kernel_selftest_cpu_topology_is_consistent(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();
	struct cpu*          bsp      = cpu_bsp();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");
	KERNEL_SELFTEST_ASSERT(ctx, topology->cpu_count > 0u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_count() == topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() >= 1u);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_online_count() <= cpu_count());
	KERNEL_SELFTEST_ASSERT_MSG(ctx, bsp != NULL, "cpu_bsp returned NULL");
	KERNEL_SELFTEST_ASSERT(ctx, topology->bsp_index < topology->cpu_count);
	KERNEL_SELFTEST_ASSERT(ctx, bsp == &topology->cpus[topology->bsp_index]);
	KERNEL_SELFTEST_ASSERT(ctx, bsp->role == CPU_ROLE_BSP);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_state_get(bsp) == CPU_STATE_ONLINE);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_current() == bsp);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_is_bsp());
	KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(bsp));
	if (cpu_count() > 1u) {
		KERNEL_SELFTEST_ASSERT_MSG(ctx, cpu_online_count() == cpu_count(), "not all discovered CPUs reached ONLINE");
	}
}

static void kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded(struct kernel_selftest_context* ctx) {
	struct cpu_topology* topology = cpu_topology_get();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology != NULL, "cpu topology is unavailable");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, topology->cpus != NULL, "cpu topology has no storage");

	for (size_t i = 0; i < topology->cpu_count; i++) {
		const struct cpu* cpu = &topology->cpus[i];

		KERNEL_SELFTEST_ASSERT(ctx, cpu->index == i);
		KERNEL_SELFTEST_ASSERT(ctx, kernel_cpu_boot_current_pointer_ok(cpu));

		for (size_t j = i + 1; j < topology->cpu_count; j++) {
			KERNEL_SELFTEST_ASSERT_MSG(
				ctx, cpu->arch_id != topology->cpus[j].arch_id, "duplicate cpu arch_id discovered during boot");
		}
	}
}

static void kernel_selftest_cpu_current_accessors_match_bound_cpu(struct kernel_selftest_context* ctx) {
	struct cpu* current = cpu_current();

	KERNEL_SELFTEST_ASSERT_MSG(ctx, current != NULL, "cpu_current returned NULL");
	KERNEL_SELFTEST_ASSERT(ctx, cpu_index() == current->index);
	KERNEL_SELFTEST_ASSERT(ctx, cpu_arch_id() == current->arch_id);
	KERNEL_SELFTEST_ASSERT(ctx, current->boot_stack_top > current->boot_stack_base);
	KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, current->exception_depth == 0u);
	KERNEL_SELFTEST_ASSERT(ctx, !cpu_irq_in_exception());
	KERNEL_SELFTEST_ASSERT(ctx, current->interrupts_ready);
}

static void kernel_selftest_cpu_irq_save_disable_tracks_nesting(struct kernel_selftest_context* ctx) {
	struct cpu*      current = cpu_current();
	struct irq_state outer;
	struct irq_state inner;
	bool             irq_was_enabled;
	bool             outer_saved = false;
	bool             inner_saved = false;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, current != NULL, "cpu_current returned NULL", cleanup);
	irq_was_enabled = irq_enabled();
	if (!irq_was_enabled) irq_enable_local();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 0u, cleanup);

	outer       = irq_save_disable();
	outer_saved = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, outer.enabled, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);

	inner       = irq_save_disable();
	inner_saved = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, inner.enabled == false, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 2u, cleanup);

	irq_restore(inner);
	inner_saved = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, !irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);

	irq_restore(outer);
	outer_saved = false;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 0u, cleanup);

cleanup:
	if (inner_saved) irq_restore(inner);
	if (outer_saved) irq_restore(outer);
	if (current != NULL && current->irq_disable_depth == 0u) {
		if (irq_was_enabled) irq_enable_local();
		else irq_disable_local();
	}
	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, irq_enabled() == irq_was_enabled);
		KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	}
}

static void kernel_selftest_cpu_spinlock_debug_checks_enforce_irqsave_and_order(struct kernel_selftest_context* ctx) {
	struct spinlock paging = SPINLOCK_INIT_CLASS("paging_lock", SPINLOCK_ORDER_PAGING, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock address_space =
		SPINLOCK_INIT_CLASS("address_space_lock", SPINLOCK_ORDER_VADDR, SPINLOCK_FLAG_IRQSAVE);
	struct spinlock  irqsave    = SPINLOCK_INIT_CLASS("clock_lock", SPINLOCK_ORDER_CLOCK, SPINLOCK_FLAG_IRQSAVE);
	struct cpu*      current    = cpu_current();
	struct irq_state state      = {0};
	bool             locked     = false;
	bool             ready_prev = false;
	bool             irq_was_enabled;

	KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, current != NULL, "cpu_current returned NULL", cleanup);
	ready_prev      = current->interrupts_ready;
	irq_was_enabled = irq_enabled();
	cpu_interrupts_set_ready(current, true);
	if (!irq_was_enabled) irq_enable_local();
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, irq_enabled(), cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, spinlock_debug_check_acquire(&irqsave) == SPINLOCK_DEBUG_CHECK_IRQSAVE_REQUIRED, cleanup);

	state  = spinlock_lock_irqsave(&paging);
	locked = true;
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, current->irq_disable_depth == 1u, cleanup);
	KERNEL_SELFTEST_ASSERT_GOTO(
		ctx, spinlock_debug_check_acquire(&address_space) == SPINLOCK_DEBUG_CHECK_ORDER, cleanup);

cleanup:
	if (locked) spinlock_unlock_irqrestore(&paging, state);
	if (current != NULL && current->irq_disable_depth == 0u) {
		if (irq_was_enabled) irq_enable_local();
		else irq_disable_local();
	}
	if (current != NULL) cpu_interrupts_set_ready(current, ready_prev);
	if (ctx->failure_expr == NULL) {
		KERNEL_SELFTEST_ASSERT(ctx, irq_enabled() == irq_was_enabled);
		KERNEL_SELFTEST_ASSERT(ctx, current->irq_disable_depth == 0u);
	}
}

struct kernel_selftest_cpu_remote_dispatch_state {
	struct cpu* expected_cpu;
	uintptr_t   actual_cpu;
	uintptr_t   current_thread;
	uint32_t    exception_depth;
	uint32_t    ran;
};

static struct kthread* kernel_selftest_cpu_remote_workers[KERNEL_SELFTEST_CPU_MAX_CPUS];
static struct kernel_selftest_cpu_remote_dispatch_state kernel_selftest_cpu_remote_states[KERNEL_SELFTEST_CPU_MAX_CPUS];
static struct sched_cpu_stats                           kernel_selftest_cpu_stats_before[KERNEL_SELFTEST_CPU_MAX_CPUS];
static struct sched_cpu_stats                           kernel_selftest_cpu_stats_after[KERNEL_SELFTEST_CPU_MAX_CPUS];
static bool kernel_selftest_cpu_remote_created[KERNEL_SELFTEST_CPU_MAX_CPUS];

static void kernel_selftest_cpu_remote_dispatch_worker(void* arg) {
	struct kernel_selftest_cpu_remote_dispatch_state* state = arg;

	if (state == NULL) return;

	__atomic_store_n(&state->actual_cpu, (uintptr_t)cpu_current(), __ATOMIC_RELEASE);
	__atomic_store_n(&state->current_thread, (uintptr_t)kthread_current(), __ATOMIC_RELEASE);
	__atomic_store_n(&state->exception_depth, cpu_current()->exception_depth, __ATOMIC_RELEASE);
	__atomic_store_n(&state->ran, 1u, __ATOMIC_RELEASE);
}

static void kernel_selftest_cpu_remote_dispatch_reaches_application_processors(struct kernel_selftest_context* ctx) {
	struct kernel_selftest_clock_scope clock        = {0};
	size_t                             total_cpus   = cpu_count();
	size_t                             worker_count = 0u;

	memset(kernel_selftest_cpu_remote_workers, 0, sizeof(kernel_selftest_cpu_remote_workers));
	memset(kernel_selftest_cpu_remote_states, 0, sizeof(kernel_selftest_cpu_remote_states));
	memset(kernel_selftest_cpu_stats_before, 0, sizeof(kernel_selftest_cpu_stats_before));
	memset(kernel_selftest_cpu_stats_after, 0, sizeof(kernel_selftest_cpu_stats_after));
	memset(kernel_selftest_cpu_remote_created, 0, sizeof(kernel_selftest_cpu_remote_created));

	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_cpus > 0u, "cpu_count returned zero");
	KERNEL_SELFTEST_ASSERT_MSG(ctx, total_cpus <= KERNEL_SELFTEST_CPU_MAX_CPUS, "cpu_count exceeds selftest capacity");

	if (total_cpus == 1u) return;
	KERNEL_SELFTEST_ASSERT_MSG_GOTO(
		ctx, kernel_selftest_clock_scope_begin(&clock), "failed to start a temporary clock source", cleanup);

	for (size_t i = 0u; i < total_cpus; i++) {
		struct cpu* cpu = cpu_by_index(i);

		KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx, cpu != NULL, "cpu_by_index returned NULL", cleanup);
		if (cpu->role != CPU_ROLE_AP) continue;
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_get_cpu_stats(cpu, &kernel_selftest_cpu_stats_before[i]), cleanup);

		kernel_selftest_cpu_remote_states[i].expected_cpu = cpu;
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(
			ctx,
			kernel_selftest_thread_create_with_preferred_cpu(&kernel_selftest_cpu_remote_workers[i],
		                                                     "selftest/cpu-remote-dispatch",
		                                                     kernel_selftest_cpu_remote_dispatch_worker,
		                                                     &kernel_selftest_cpu_remote_states[i],
		                                                     cpu),
			"failed to create AP-targeted worker thread",
			cleanup);
		kernel_selftest_cpu_remote_created[i] = true;
		worker_count++;
	}

	/* Blocking the BSP lets single-threaded emulators schedule each AP. */
	for (size_t i = 0u; i < total_cpus; i++) {
		if (!kernel_selftest_cpu_remote_created[i]) continue;
		KERNEL_SELFTEST_ASSERT_MSG_GOTO(ctx,
		                                kthread_timed_join(kernel_selftest_cpu_remote_workers[i],
		                                                   KERNEL_SELFTEST_CPU_REMOTE_DISPATCH_TIMEOUT_MS,
		                                                   NULL),
		                                "AP-targeted worker did not finish before the dispatch timeout",
		                                cleanup);
	}

	for (size_t i = 0u; i < total_cpus; i++) {
		struct cpu* cpu = kernel_selftest_cpu_remote_states[i].expected_cpu;

		if (!kernel_selftest_cpu_remote_created[i]) continue;

		KERNEL_SELFTEST_ASSERT_GOTO(
			ctx, __atomic_load_n(&kernel_selftest_cpu_remote_states[i].ran, __ATOMIC_ACQUIRE) != 0u, cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(
			ctx,
			(struct cpu*)__atomic_load_n(&kernel_selftest_cpu_remote_states[i].actual_cpu, __ATOMIC_ACQUIRE) == cpu,
			cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(
			ctx,
			(struct thread*)__atomic_load_n(&kernel_selftest_cpu_remote_states[i].current_thread, __ATOMIC_ACQUIRE) ==
				&kernel_selftest_cpu_remote_workers[i]->thread,
			cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(
			ctx,
			__atomic_load_n(&kernel_selftest_cpu_remote_states[i].exception_depth, __ATOMIC_ACQUIRE) == 0u,
			cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx, sched_get_cpu_stats(cpu, &kernel_selftest_cpu_stats_after[i]), cleanup);
		KERNEL_SELFTEST_ASSERT_GOTO(ctx,
		                            kernel_selftest_cpu_stats_after[i].context_switch_count >
		                                kernel_selftest_cpu_stats_before[i].context_switch_count,
		                            cleanup);
	}
	KERNEL_SELFTEST_ASSERT_GOTO(ctx, worker_count == cpu_online_count() - 1u, cleanup);

cleanup:
	for (size_t i = 0u; i < total_cpus && i < KERNEL_SELFTEST_CPU_MAX_CPUS; i++) {
		if (!kernel_selftest_cpu_remote_created[i]) continue;
		if (kernel_selftest_thread_is_live(kernel_selftest_cpu_remote_workers[i])) {
			kernel_selftest_dispatch_rounds(KERNEL_SELFTEST_MAX_DISPATCH_ROUNDS);
		}
		kernel_selftest_thread_destroy(&kernel_selftest_cpu_remote_workers[i]);
	}
	kernel_selftest_clock_scope_end(&clock);
}

static const struct kernel_selftest_case kernel_cpu_selftests[] = {
#if defined(PLATFORM_PC_X86_64)
	{.name = "x86_interrupt_ranges_are_stable", .run = kernel_selftest_cpu_x86_interrupt_ranges_are_stable},
#endif
#if defined(PLATFORM_PC_AARCH64)
	{.name = "gic_external_source_contract", .run = kernel_selftest_cpu_gic_external_source_contract},
#endif
#if defined(PLATFORM_PC_RISCV64)
	{.name = "plic_firmware_source_contract", .run = kernel_selftest_cpu_plic_firmware_source_contract},
	{.name = "aplic_source_starts_masked", .run = kernel_selftest_cpu_aplic_source_starts_masked},
	{.name = "imsic_message_contract", .run = kernel_selftest_cpu_imsic_message_contract},
#endif
#if defined(PLATFORM_PC_LOONGARCH64)
	{.name = "loongarch_interrupt_contract", .run = kernel_selftest_cpu_loongarch_interrupt_contract},
#endif
	{.name = "interrupt_source_lifecycle", .run = kernel_selftest_cpu_interrupt_source_lifecycle},
	{.name = "interrupt_message_lifecycle", .run = kernel_selftest_cpu_interrupt_message_lifecycle},
	{
								   .name = "topology_is_consistent",
								   .run  = kernel_selftest_cpu_topology_is_consistent,
								   },
	{
								   .name = "ids_are_unique_and_bindings_succeeded",
								   .run  = kernel_selftest_cpu_ids_are_unique_and_bindings_succeeded,
								   },
	{
								   .name = "current_accessors_match_bound_cpu",
								   .run  = kernel_selftest_cpu_current_accessors_match_bound_cpu,
								   },
	{
								   .name = "irq_save_disable_tracks_nesting",
								   .run  = kernel_selftest_cpu_irq_save_disable_tracks_nesting,
								   },
	{
								   .name = "spinlock_debug_checks_enforce_irqsave_and_order",
								   .run  = kernel_selftest_cpu_spinlock_debug_checks_enforce_irqsave_and_order,
								   },
	{
								   .name = "remote_dispatch_reaches_application_processors",
								   .run  = kernel_selftest_cpu_remote_dispatch_reaches_application_processors,
								   },
	{
								   .name = "global_executable_sync_reaches_online_cpus",
								   .run  = kernel_selftest_cpu_global_executable_sync,
								   },
};

const struct kernel_selftest_suite kernel_cpu_selftest_suite = {
	.name       = "cpu",
	.cases      = kernel_cpu_selftests,
	.case_count = sizeof(kernel_cpu_selftests) / sizeof(kernel_cpu_selftests[0]),
};
