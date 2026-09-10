#pragma once

#include <hal/iommu.h>

/* Initialize one firmware-described LoongArch IOMMUv1 controller. */
bool loongarch_iommu_v1_controller_init(struct hal_iommu_controller_state*            controller,
                                        const struct hal_iommu_controller_descriptor* descriptor,
                                        struct hal_iommu_info*                        out_info);

/* Disable one unused LoongArch IOMMUv1 controller. */
void loongarch_iommu_v1_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether IOMMUv1 can enforce the requested access exactly. */
bool loongarch_iommu_v1_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize one empty IOMMUv1 translation space. */
bool loongarch_iommu_v1_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                                   struct hal_iommu_space_state* space);

/* Release one empty IOMMUv1 translation space. */
void loongarch_iommu_v1_space_deinit(struct hal_iommu_controller_state* controller,
                                     struct hal_iommu_space_state*      space);

/* Map one contiguous physical extent through IOMMUv1. */
bool loongarch_iommu_v1_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                            const struct hal_iommu_map_request* request);

/* Unmap one completely translated IOMMUv1 range. */
bool loongarch_iommu_v1_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                              uint64_t io_address, size_t size);

/* Attach one source to an IOMMUv1 translation space. */
bool loongarch_iommu_v1_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                               uint32_t source_id, struct hal_iommu_attachment_state* attachment);

/* Detach one source and leave it blocked. */
bool loongarch_iommu_v1_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                               struct hal_iommu_attachment_state* attachment);
