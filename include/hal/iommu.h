#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum hal_iommu_access {
	HAL_IOMMU_READ  = 1u << 0,
	HAL_IOMMU_WRITE = 1u << 1,
};

#define HAL_IOMMU_ACCESS_VALID_MASK ((uint64_t)(HAL_IOMMU_READ | HAL_IOMMU_WRITE))

/* Hardware architecture implemented by one discovered IOMMU controller. */
enum hal_iommu_kind {
	HAL_IOMMU_KIND_INTEL_VTD = 0,
	HAL_IOMMU_KIND_AMD,
	HAL_IOMMU_KIND_ARM_SMMUV3,
	HAL_IOMMU_KIND_RISCV,
	HAL_IOMMU_KIND_LOONGARCH_V1,
};

/* Immutable translation geometry of one initialized IOMMU controller. */
struct hal_iommu_info {
	enum hal_iommu_kind kind;
	size_t              minimum_leaf_size;
	uint64_t            leaf_size_mask;
	uint8_t             io_address_bits;
	uint8_t             physical_address_bits;
	uint8_t             context_id_bits;
	uint8_t             source_id_bits;
};

/* Immutable platform description of one discovered IOMMU controller. */
struct hal_iommu_controller_descriptor {
	enum hal_iommu_kind kind;
	uintptr_t           register_address;
#if defined(PLATFORM_PC_X86_64)
	uint8_t firmware_flags;
	uint8_t firmware_io_address_bits;
	uint8_t firmware_physical_address_bits;
#endif
#if defined(PLATFORM_PC_LOONGARCH64)
	uint64_t firmware_leaf_size_mask;
	uint32_t register_size;
	uint32_t maximum_source_count;
	uint32_t firmware_flags;
	uint32_t device_id;
	uint16_t segment;
	uint16_t firmware_io_address_bits;
	uint16_t firmware_physical_address_bits;
	uint16_t firmware_page_table_levels;
#endif
#if defined(HAL_IOMMU_MOCK)
	struct hal_iommu_info mock_info;
#endif
};

/* Common concrete state of one architecture-owned IOMMU page-table hierarchy. */
struct hal_iommu_page_table_state {
	bool      initialized;
	uintptr_t root_address;
	size_t    table_allocation_size;
	uint64_t  mapped_size;
	uintptr_t controller_identity;
	uint32_t  context_id;
	uint8_t   levels;
};

#if defined(PLATFORM_PC_X86_64)
/* Concrete state of one x86 Intel or AMD DMA-remapping controller. */
struct hal_iommu_controller_state {
	uint32_t            operation_lock;
	enum hal_iommu_kind kind;
	bool                initialized;
	volatile void*      registers;
	uintptr_t           source_table_address;
	uintptr_t           command_queue_address;
	size_t              table_allocation_size;
	size_t              source_table_size;
	size_t              command_queue_size;
	uint64_t            capabilities;
	uint64_t            extended_capabilities;
	uint64_t            control;
	uint64_t            leaf_size_mask;
	uint64_t            address_mask;
	uint16_t            context_limit;
	uint32_t            command;
	uint32_t            command_queue_tail;
	uint8_t             io_address_bits;
	uint8_t             physical_address_bits;
	uint8_t             context_id_bits;
	uint8_t             source_id_bits;
};

#elif defined(PLATFORM_PC_AARCH64)

/* Concrete state of one Arm SMMUv3 controller. */
struct hal_iommu_controller_state {
	uint32_t       operation_lock;
	bool           initialized;
	volatile void* registers;
	uintptr_t      stream_table_address;
	uintptr_t      command_queue_address;
	uintptr_t      event_queue_address;
	size_t         table_allocation_size;
	size_t         stream_table_size;
	size_t         command_queue_size;
	size_t         event_queue_size;
	uint64_t       leaf_size_mask;
	uint64_t       address_mask;
	uint32_t       stream_count;
	uint32_t       command_queue_producer;
	uint8_t        io_address_bits;
	uint8_t        physical_address_bits;
	uint8_t        context_id_bits;
	uint8_t        source_id_bits;
	uint8_t        command_queue_log2_entries;
	uint8_t        event_queue_log2_entries;
	uint8_t        stream_table_split;
	bool           stream_table_two_level;
};

#elif defined(PLATFORM_PC_RISCV64)

/* Concrete state of one RISC-V IOMMU controller. */
struct hal_iommu_controller_state {
	uint32_t       operation_lock;
	bool           initialized;
	volatile void* registers;
	uintptr_t      device_directory_address;
	uintptr_t      command_queue_address;
	size_t         table_allocation_size;
	size_t         device_directory_size;
	size_t         command_queue_size;
	uint64_t       capabilities;
	uint64_t       leaf_size_mask;
	uint64_t       address_mask;
	uint32_t       device_count;
	uint32_t       command_queue_tail;
	uint8_t        io_address_bits;
	uint8_t        physical_address_bits;
	uint8_t        context_id_bits;
	uint8_t        source_id_bits;
	uint8_t        command_queue_log2_entries;
	uint8_t        device_directory_levels;
	uint8_t        device_context_size;
};

#elif defined(PLATFORM_PC_LOONGARCH64)

/* Concrete state reserved for a discovered LoongArch IOMMUv1 controller. */
struct hal_iommu_controller_state {
	bool initialized;
};

#elif defined(HAL_IOMMU_MOCK)

/* Concrete hosted-test controller state. */
struct hal_iommu_controller_state {
	uint32_t              operation_lock;
	bool                  initialized;
	struct hal_iommu_info info;
	void*                 source_entries;
	size_t                source_count;
	size_t                source_capacity;
	size_t                live_spaces;
	size_t                invalidations;
};

#else
#error "No IOMMU state layout selected"
#endif

/* Concrete state of one core-owned IOMMU translation space. */
struct hal_iommu_space_state {
	struct hal_iommu_page_table_state table;
#if defined(HAL_IOMMU_MOCK)
	void*  leaves;
	size_t leaf_count;
#endif
};

/* Parameters for mapping one contiguous physical extent into an I/O address space. */
struct hal_iommu_map_request {
	uint64_t  io_address;
	uintptr_t physical_address;
	size_t    size;
	uint64_t  access;
};

/* Return the number of IOMMU controllers described by platform firmware. */
size_t hal_iommu_controller_count(void);

/* Return one discovered controller descriptor by stable platform order. */
bool hal_iommu_controller_at(size_t index, struct hal_iommu_controller_descriptor* out_descriptor);

/* Convert an IOMMU hardware kind to a short diagnostic name. */
static inline const char* hal_iommu_kind_string(enum hal_iommu_kind kind) {
	switch (kind) {
	case HAL_IOMMU_KIND_INTEL_VTD:
		return "Intel VT-d";
	case HAL_IOMMU_KIND_AMD:
		return "AMD IOMMU";
	case HAL_IOMMU_KIND_ARM_SMMUV3:
		return "Arm SMMUv3";
	case HAL_IOMMU_KIND_RISCV:
		return "RISC-V IOMMU";
	case HAL_IOMMU_KIND_LOONGARCH_V1:
		return "LoongArch IOMMUv1";
	}
	return "unknown";
}

/* Initialize one physical IOMMU controller and return its immutable geometry. */
bool hal_iommu_controller_init(struct hal_iommu_controller_state*            controller,
                               const struct hal_iommu_controller_descriptor* descriptor,
                               struct hal_iommu_info*                        out_info);

/* Disable one unused controller and release all controller-owned architectural memory. */
void hal_iommu_controller_deinit(struct hal_iommu_controller_state* controller);

/* Return whether one device access mask can be enforced exactly by the controller. */
bool hal_iommu_mapping_supported(const struct hal_iommu_controller_state* controller, uint64_t access);

/* Initialize one empty translation space using a core-selected context identifier. */
bool hal_iommu_space_init(struct hal_iommu_controller_state* controller, uint32_t context_id,
                          struct hal_iommu_space_state* space);

/* Release the empty, detached translation space and its architectural tables. */
void hal_iommu_space_deinit(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space);

/* Map one complete contiguous physical extent transactionally. */
bool hal_iommu_map(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                   const struct hal_iommu_map_request* request);

/* Remove one completely translated I/O range transactionally. */
bool hal_iommu_unmap(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                     uint64_t io_address, size_t size);

/* Attach one local hardware source to a translation space. */
bool hal_iommu_attach(struct hal_iommu_controller_state* controller, struct hal_iommu_space_state* space,
                      uint32_t source_id);

/* Detach one local hardware source and leave it blocked. */
bool hal_iommu_detach(struct hal_iommu_controller_state* controller, uint32_t source_id);
