#pragma once

#include <hal/iommu.h>

/* Initialize an Arm SMMUv3 controller. */
bool aarch64_smmuv3_controller_init(struct hal_iommu_controller_state*            controller,
                                    const struct hal_iommu_controller_descriptor* descriptor,
                                    struct hal_iommu_info*                        out_info);

/* Disable an Arm SMMUv3 controller and release its resources. */
void aarch64_smmuv3_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether SMMUv3 can enforce an access mask exactly. */
bool aarch64_smmuv3_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize an Arm SMMUv3 translation space. */
bool aarch64_smmuv3_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                               struct hal_iommu_space_state* space);

/* Release an empty Arm SMMUv3 translation space. */
void aarch64_smmuv3_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space);

/* Map a physical extent through an Arm SMMUv3. */
bool aarch64_smmuv3_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                        const struct hal_iommu_map_request* request);

/* Unmap an I/O address range from an Arm SMMUv3. */
bool aarch64_smmuv3_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                          uint64_t io_address, size_t size);

/* Attach a hardware stream to an Arm SMMUv3 translation space. */
bool aarch64_smmuv3_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                           uint32_t source_id, struct hal_iommu_attachment_state* attachment);

/* Detach and block a hardware stream from an Arm SMMUv3. */
bool aarch64_smmuv3_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                           struct hal_iommu_attachment_state* attachment);
