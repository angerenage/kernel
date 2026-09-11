#pragma once

#include <hal/iommu.h>

/* Initialize a RISC-V IOMMU controller. */
bool riscv_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                                 const struct hal_iommu_controller_descriptor* descriptor,
                                 struct hal_iommu_info*                        out_info);

/* Disable a RISC-V IOMMU controller and release its resources. */
void riscv_iommu_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether the RISC-V IOMMU can enforce an access mask exactly. */
bool riscv_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize a RISC-V IOMMU translation space. */
bool riscv_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                            struct hal_iommu_space_state* space);

/* Release an empty RISC-V IOMMU translation space. */
void riscv_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space);

/* Map a physical extent through a RISC-V IOMMU. */
bool riscv_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     const struct hal_iommu_map_request* request);

/* Unmap an I/O address range from a RISC-V IOMMU. */
bool riscv_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                       uint64_t io_address, size_t size);

/* Change permissions on an I/O address range for a RISC-V IOMMU. */
bool riscv_iommu_protect(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                         uint64_t io_address, size_t size, uint64_t access);

/* Attach a hardware source to a RISC-V IOMMU translation space. */
bool riscv_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                        uint32_t source_id, struct hal_iommu_attachment_state* attachment);

/* Detach and block a hardware source from a RISC-V IOMMU. */
bool riscv_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                        struct hal_iommu_attachment_state* attachment);
