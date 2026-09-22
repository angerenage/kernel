#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Architecture delivery namespaces used by RISC-V interrupt controllers. */
enum riscv64_interrupt_delivery_domain {
	RISCV64_DELIVERY_DOMAIN_PLIC = 1u,
	RISCV64_DELIVERY_DOMAIN_IMSIC,
	RISCV64_DELIVERY_DOMAIN_APLIC,
};

/* Message-producing mechanisms exposed by the RISC-V HAL. */
enum riscv64_interrupt_message_domain {
	RISCV64_MESSAGE_DOMAIN_IMSIC = 1u,
};

/* Resolve a firmware interrupt-controller phandle to a hardware thread ID. */
bool riscv64_interrupt_hart_for_phandle(uint32_t wanted, uint64_t* out_hart);

/* Synchronize the local external-interrupt controller state. */
void riscv64_sync_external_interrupt_local(void);
