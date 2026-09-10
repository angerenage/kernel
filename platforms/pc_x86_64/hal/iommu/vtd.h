#pragma once

#include <hal/iommu.h>

/* Initialize an Intel VT-d controller. */
bool x86_vtd_controller_init(struct hal_iommu_controller_state*            controller,
                             const struct hal_iommu_controller_descriptor* descriptor, struct hal_iommu_info* out_info);

/* Disable an Intel VT-d controller and release its resources. */
void x86_vtd_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether VT-d can enforce an access mask exactly. */
bool x86_vtd_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize an Intel VT-d translation space. */
bool x86_vtd_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                        struct hal_iommu_space_state* space);

/* Release an empty Intel VT-d translation space. */
void x86_vtd_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space);

/* Map a physical extent through Intel VT-d. */
bool x86_vtd_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                 const struct hal_iommu_map_request* request);

/* Unmap an I/O address range from Intel VT-d. */
bool x86_vtd_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   uint64_t io_address, size_t size);

/* Attach a hardware source to an Intel VT-d translation space. */
bool x86_vtd_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                    uint32_t source_id, struct hal_iommu_attachment_state* attachment);

/* Detach and block a hardware source from Intel VT-d. */
bool x86_vtd_detach(struct hal_iommu_controller_state* controller, uint32_t source_id,
                    struct hal_iommu_attachment_state* attachment);
