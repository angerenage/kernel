#pragma once

#include <hal/interrupts.h>
#include <stdbool.h>
#include <stdint.h>

struct cpu;

/* Return whether a CPU has a firmware-described IMSIC interrupt file. */
bool riscv64_imsic_cpu_has_interface(const struct cpu* cpu);

/* Return whether a firmware phandle identifies the discovered IMSIC. */
bool riscv64_imsic_is_parent(uint32_t phandle);

/* Return the firmware-controller index selected for the supervisor IMSIC. */
bool riscv64_imsic_selected_controller(size_t* out_controller);

/* Resolve a CPU and interrupt identity to an IMSIC hart index. */
bool riscv64_imsic_target(const struct cpu* cpu, uint32_t id, uint32_t* out_hart_index);

/* Return the message-signaled interrupt range exposed by the IMSIC. */
bool riscv64_imsic_message_range_at(struct hal_interrupt_message_range* out);

/* Return whether the IMSIC message mechanism can deliver to a CPU without changing hardware state. */
bool riscv64_imsic_message_target_supported(uint32_t domain, const struct hal_interrupt_message_source* source,
                                            const struct cpu* target);

/* Initialize one core-reserved IMSIC message identity. */
bool riscv64_imsic_message_init(struct hal_interrupt_message_state*         state,
                                const struct hal_interrupt_message_request* request,
                                struct hal_interrupt_message*               out_message);

/* Disable architecture programming for one IMSIC message identity. */
bool riscv64_imsic_message_deinit(struct hal_interrupt_message_state* state);

/* Apply the desired IMSIC enable state to the local interrupt file. */
void riscv64_imsic_sync_local(void);

/* Change one IMSIC identity's desired enable state, waiting for a remote target to apply the update. */
bool riscv64_imsic_set_enabled(const struct cpu* cpu, uint32_t id, bool enabled);

/* Claim and dispatch one pending IMSIC interrupt identity. */
bool riscv64_imsic_handle_external_irq(void);
