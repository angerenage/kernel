#pragma once

#include <stdint.h>

/* TPM command protocol family, when firmware identifies it. */
enum tpm_family {
	TPM_FAMILY_UNSPECIFIED = 0,
	TPM_FAMILY_1_2,
	TPM_FAMILY_2_0,
	TPM_FAMILY_COUNT,
};

/* Hardware interface used to exchange commands with a TPM. */
enum tpm_interface {
	TPM_INTERFACE_INVALID = 0,
	TPM_INTERFACE_FIFO_MMIO,
	TPM_INTERFACE_CRB,
	TPM_INTERFACE_COUNT,
};

struct tpm_fifo_mmio_interface {
	uint64_t register_address;
	uint64_t register_size;
};

struct tpm_crb_interface {
	uint64_t control_area_address;
};

/* Interface-specific access information, padded for future interface types. */
union tpm_access {
	struct tpm_fifo_mmio_interface fifo_mmio;
	struct tpm_crb_interface       crb;
	uint64_t                       reserved[4];
};

/* Source-neutral description of one Trusted Platform Module. */
struct tpm_device {
	enum tpm_family    family;
	enum tpm_interface interface;
	union tpm_access   access;
};
