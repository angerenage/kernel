#include "eiointc.h"

#include <core/cpu.h>
#include <stdbool.h>
#include <stdint.h>

#include "controller.h"
#include "pch_pic.h"

#define IOCSR_FEATURES 0x8u
#define IOCSR_FEATURE_EXTIOI (1ull << 3)
#define IOCSR_MISC_FUNC 0x420u
#define IOCSR_MISC_FUNC_EXTIOI_EN (1ull << 48)
#define EIOINTC_NODEMAP 0x14a0u
#define EIOINTC_IPMAP 0x14c0u
#define EIOINTC_ENABLE 0x1600u
#define EIOINTC_BOUNCE 0x1680u
#define EIOINTC_ISR 0x1800u
#define EIOINTC_ROUTE 0x1c00u

static inline uint32_t iocsr_read32(uint32_t address) {
	uint32_t value;
	__asm__ volatile("iocsrrd.w %0, %1" : "=r"(value) : "r"((uint64_t)address));
	return value;
}

static inline uint64_t iocsr_read64(uint32_t address) {
	uint64_t value;
	__asm__ volatile("iocsrrd.d %0, %1" : "=r"(value) : "r"((uint64_t)address));
	return value;
}

static inline void iocsr_write32(uint32_t value, uint32_t address) {
	__asm__ volatile("iocsrwr.w %0, %1" : : "r"(value), "r"((uint64_t)address) : "memory");
}

static inline void iocsr_write64(uint64_t value, uint32_t address) {
	__asm__ volatile("iocsrwr.d %0, %1" : : "r"(value), "r"((uint64_t)address) : "memory");
}

void loongarch64_eiointc_route(uint32_t vector, const struct cpu* cpu) {
	if (!eiointc.described || vector >= eiointc.vector_count || cpu == NULL || cpu->arch_id >= 64u) return;
	uint32_t address = EIOINTC_ROUTE + (vector & ~3u);
	uint32_t shift   = (vector & 3u) * 8u;
	uint8_t  route   = (uint8_t)((1u << (cpu->arch_id & 3u)) | ((cpu->arch_id / 4u) << 4u));
	uint32_t value   = iocsr_read32(address);
	value            = (value & ~(0xffu << shift)) | ((uint32_t)route << shift);
	iocsr_write32(value, address);
}

void loongarch64_eiointc_set_enabled(uint32_t vector, bool enabled) {
	if (!eiointc.described || vector >= eiointc.vector_count) return;
	uint32_t offset = EIOINTC_ENABLE + (vector / 32u) * 4u;
	uint32_t value  = iocsr_read32(offset);
	if (enabled) value |= 1u << (vector % 32u);
	else value &= ~(1u << (vector % 32u));
	iocsr_write32(value, offset);
}

bool loongarch64_eiointc_init_local(const struct cpu* cpu) {
	if (cpu == NULL) return false;
	if (!eiointc.described) return true;
	if ((iocsr_read64(IOCSR_FEATURES) & IOCSR_FEATURE_EXTIOI) == 0u) return false;
	uint64_t misc = iocsr_read64(IOCSR_MISC_FUNC);
	iocsr_write64(misc | IOCSR_MISC_FUNC_EXTIOI_EN, IOCSR_MISC_FUNC);
	if ((cpu->arch_id & 3u) != 0u) return true;
	for (uint32_t word = 0u; word < eiointc.vector_count / 32u; word++) {
		iocsr_write32((1u << ((word * 2u + 1u) & 31u)) << 16u | (1u << ((word * 2u) & 31u)),
		              EIOINTC_NODEMAP + word * 4u);
		iocsr_write32(0u, EIOINTC_ENABLE + word * 4u);
		iocsr_write32(UINT32_MAX, EIOINTC_BOUNCE + word * 4u);
	}
	uint32_t pin = 1u << (eiointc.cascade - LOONGARCH64_CPU_HWI_BASE);
	uint32_t map = pin | (pin << 8u) | (pin << 16u) | (pin << 24u);
	for (uint32_t group = 0u; group < eiointc.vector_count / 128u; group++)
		iocsr_write32(map, EIOINTC_IPMAP + group * 4u);
	uint8_t  route      = (uint8_t)(1u << (cpu->arch_id & 3u));
	uint32_t route_word = route | ((uint32_t)route << 8u) | ((uint32_t)route << 16u) | ((uint32_t)route << 24u);
	for (uint32_t vector = 0u; vector < eiointc.vector_count; vector += 4u)
		iocsr_write32(route_word, EIOINTC_ROUTE + vector);
	return true;
}

bool loongarch64_eiointc_handle(void) {
	bool handled = false;
	for (uint32_t word = 0u; word < eiointc.vector_count / 32u; word++) {
		uint32_t pending = iocsr_read32(EIOINTC_ISR + word * 4u);
		if (pending == 0u) continue;
		iocsr_write32(pending, EIOINTC_ISR + word * 4u);
		while (pending != 0u) {
			uint32_t bit    = (uint32_t)__builtin_ctz(pending);
			uint32_t vector = word * 32u + bit;
			(void)loongarch64_pch_pic_mask_vector_leaf(vector);
			pending &= ~(1u << bit);
		}
		handled = true;
	}
	return handled;
}
