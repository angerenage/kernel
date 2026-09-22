#include "avec.h"

#include <core/cpu.h>
#include <hal/interrupts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller.h"

#define IOCSR_FEATURES 0x8u
#define IOCSR_FEATURE_AVEC (1ull << 15)
#define IOCSR_FEATURE_REDIRECT (1ull << 16)
#define IOCSR_MISC_FUNC 0x420u
#define IOCSR_MISC_FUNC_AVEC_EN (1ull << 51)
#define AVEC_IRR_VECTOR_MASK 0xffu
#define AVEC_IRR_INVALID (1u << 31)
#define AVEC_MESSAGE_OFFSET 0x100000u
#define AVEC_VECTOR_SHIFT 4u
#define AVEC_CPU_SHIFT 12u
#define AVEC_CPU_LIMIT (1u << 16)

static bool avec_available;
static bool avec_redirect;
static bool avec_local_ready[64];

static inline uint64_t iocsr_read64(uint32_t address) {
	uint64_t value;
	__asm__ volatile("iocsrrd.d %0, %1" : "=r"(value) : "r"((uint64_t)address));
	return value;
}

static inline void iocsr_write64(uint64_t value, uint32_t address) {
	__asm__ volatile("iocsrwr.d %0, %1" : : "r"(value), "r"((uint64_t)address) : "memory");
}

static inline uint64_t csr_read_irr(void) {
	uint64_t value;
	__asm__ volatile("csrrd %0, 0xa4" : "=r"(value));
	return value;
}

void loongarch64_avec_discover(void) {
	uint64_t features = iocsr_read64(IOCSR_FEATURES);
	avec_available    = pch_msi.described && pch_msi.address >= AVEC_MESSAGE_OFFSET &&
	                    pch_msi.address - AVEC_MESSAGE_OFFSET <= UINT32_MAX && (features & IOCSR_FEATURE_AVEC) != 0u;
	avec_redirect     = avec_available && (features & IOCSR_FEATURE_REDIRECT) != 0u;
	for (size_t index = 0u; index < 64u; index++) avec_local_ready[index] = false;
}

bool loongarch64_avec_init_local(const struct cpu* cpu) {
	if (!avec_available) return true;
	if (cpu == NULL || cpu->index >= 64u || cpu->arch_id >= AVEC_CPU_LIMIT) return false;
	uint64_t misc = iocsr_read64(IOCSR_MISC_FUNC);
	iocsr_write64(misc | IOCSR_MISC_FUNC_AVEC_EN, IOCSR_MISC_FUNC);
	avec_local_ready[cpu->index] = true;
	return true;
}

bool loongarch64_avec_available(void) {
	return avec_available;
}

bool loongarch64_avec_uses_redirect(void) {
	return avec_redirect;
}

bool loongarch64_avec_cpu_supported(const struct cpu* target) {
	if (!avec_available || target == NULL || target->index >= 64u || target->arch_id >= AVEC_CPU_LIMIT) return false;
	return avec_local_ready[target->index] && target->interrupts_ready;
}

bool loongarch64_avec_range(struct hal_interrupt_message_range* out_range) {
	if (!avec_available || out_range == NULL) return false;
	*out_range = (struct hal_interrupt_message_range){
		.domain   = avec_redirect ? LOONGARCH64_MESSAGE_DOMAIN_REDIRECT : LOONGARCH64_MESSAGE_DOMAIN_AVEC,
		.delivery = {.domain = LOONGARCH64_DELIVERY_DOMAIN_AVEC, .base = AVEC_VECTOR_BASE, .limit = AVEC_VECTOR_COUNT}
    };
	return true;
}

bool loongarch64_avec_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                       const struct cpu* target) {
	if (avec_redirect || domain != LOONGARCH64_MESSAGE_DOMAIN_AVEC || source != NULL ||
	    !loongarch64_avec_cpu_supported(target))
		return false;
	return true;
}

bool loongarch64_avec_init(struct hal_interrupt_message_state*         state,
                           const struct hal_interrupt_message_request* request,
                           struct hal_interrupt_message*               out_message) {
	struct hal_interrupt_message_range range;
	if (state == NULL || state->initialized || request == NULL || out_message == NULL ||
	    !loongarch64_avec_target_supported(request->domain, request->source, request->target) ||
	    !loongarch64_avec_range(&range) || request->event.domain != range.delivery.domain ||
	    request->event.id < range.delivery.base || request->event.id >= range.delivery.limit)
		return false;
	uint64_t address = pch_msi.address - AVEC_MESSAGE_OFFSET;
	address |= (uint64_t)request->event.id << AVEC_VECTOR_SHIFT;
	address |= request->target->arch_id << AVEC_CPU_SHIFT;
	*out_message = (struct hal_interrupt_message){.address = address, .data = 0u};
	*state       = (struct hal_interrupt_message_state){
		.domain = LOONGARCH64_MESSAGE_DOMAIN_AVEC, .event = request->event, .initialized = true};
	return true;
}

bool loongarch64_avec_deinit(struct hal_interrupt_message_state* state) {
	if (state == NULL) return false;
	if (!state->initialized) return true;
	if (state->domain != LOONGARCH64_MESSAGE_DOMAIN_AVEC || state->event.domain != LOONGARCH64_DELIVERY_DOMAIN_AVEC)
		return false;
	state->initialized = false;
	return true;
}

bool loongarch64_avec_handle(void) {
	if (!avec_available) return false;
	bool handled = false;
	for (;;) {
		uint64_t request = csr_read_irr();
		if ((request & AVEC_IRR_INVALID) != 0u) break;
		(void)(request & AVEC_IRR_VECTOR_MASK);
		handled = true;
	}
	return handled;
}
