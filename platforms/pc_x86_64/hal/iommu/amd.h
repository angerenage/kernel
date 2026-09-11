#pragma once

#include <hal/iommu.h>

/* Initialize an AMD IOMMU controller. */
bool x86_amd_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                                   const struct hal_iommu_controller_descriptor* descriptor,
                                   struct hal_iommu_info*                        out_info);

/* Disable an AMD IOMMU controller and release its resources. */
void x86_amd_iommu_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether the AMD IOMMU can enforce an access mask exactly. */
bool x86_amd_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize an AMD IOMMU translation space. */
bool x86_amd_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                              struct hal_iommu_space_state* space);

/* Release an empty AMD IOMMU translation space. */
void x86_amd_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space);

/* Map a physical extent through an AMD IOMMU. */
bool x86_amd_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                       const struct hal_iommu_map_request* request);

/* Unmap an I/O address range from an AMD IOMMU. */
bool x86_amd_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                         uint64_t io_address, size_t size);

/* Change permissions on an I/O address range for an AMD IOMMU. */
bool x86_amd_iommu_protect(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                           uint64_t io_address, size_t size, uint64_t access);

/* Attach a hardware source to an AMD IOMMU translation space. */
bool x86_amd_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                          uint32_t source_id, struct hal_iommu_attachment_state* attachment);

/* Detach and block a hardware source from an AMD IOMMU. */
bool x86_amd_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                          struct hal_iommu_attachment_state* attachment);
