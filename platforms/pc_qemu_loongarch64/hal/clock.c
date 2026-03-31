#include <hal/clock.h>
#include <hal/hcf.h>
#include <kernel/requests.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LOONGARCH64_CSR_ECFG 0x4u
#define LOONGARCH64_CSR_EENTRY 0xcu
#define LOONGARCH64_CSR_TLBRENTRY 0x88u
#define LOONGARCH64_CSR_MERRENTRY 0x94u

static bool clock_initialized;
extern void loongarch64_exception_entry(void);
extern void loongarch64_tlb_refill_entry(void);
extern void loongarch64_machine_error_entry(void);

static uintptr_t loongarch64_kernel_virt_to_phys(const void* ptr) {
	if (exec_addr_req.response == NULL) {
		return 0;
	}

	return (uintptr_t)(exec_addr_req.response->physical_base +
	                   ((uint64_t)(uintptr_t)ptr - exec_addr_req.response->virtual_base));
}

void hal_clock_init(void) {
	if (clock_initialized) return;

	uintptr_t exception_entry = (uintptr_t)loongarch64_exception_entry;
	uintptr_t tlbr_entry      = loongarch64_kernel_virt_to_phys((const void*)loongarch64_tlb_refill_entry);
	uintptr_t merr_entry      = loongarch64_kernel_virt_to_phys((const void*)loongarch64_machine_error_entry);
	uintptr_t zero            = 0;

	if (exec_addr_req.response == NULL || tlbr_entry == 0 || merr_entry == 0) {
		printf("kernel: loongarch64 kernel address response missing for trap setup\n");
		hcf();
	}

	__asm__ volatile("csrwr %0, %1" : : "r"(zero), "i"(LOONGARCH64_CSR_ECFG) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(exception_entry), "i"(LOONGARCH64_CSR_EENTRY) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(tlbr_entry), "i"(LOONGARCH64_CSR_TLBRENTRY) : "memory");
	__asm__ volatile("csrwr %0, %1" : : "r"(merr_entry), "i"(LOONGARCH64_CSR_MERRENTRY) : "memory");

	clock_initialized = true;
	printf("kernel: loongarch64 trap entries installed (eentry=%p tlbrentry=%p merrentry=%p)\n",
	       (void*)exception_entry,
	       (void*)tlbr_entry,
	       (void*)merr_entry);
}

bool hal_clock_start(uint32_t frequency_hz, hal_clock_handler_t handler, void* ctx) {
	(void)frequency_hz;
	(void)handler;
	(void)ctx;

	if (!clock_initialized || frequency_hz == 0u || handler == NULL) return false;
	return false;
}

uint32_t hal_clock_frequency(void) {
	return 0u;
}

void hal_clock_stop(void) {
}
