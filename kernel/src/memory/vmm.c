#include <core/pmm.h>
#include <core/spinlock.h>
#include <core/vaddr_alloc.h>
#include <core/vmm.h>
#include <hal/paging.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KERNEL_HEAP_BASE 0xffffffffa0000000ull
#define KERNEL_HEAP_SIZE 0x40000000ull

static bool            initialized;
static struct spinlock vmm_lock = SPINLOCK_INIT;

static void rollback_mapping(uintptr_t virt_base, size_t mapped_pages, size_t reserved_pages) {
	for (size_t page = 0; page < mapped_pages; page++) {
		uintptr_t phys = 0;
		uintptr_t virt = virt_base + page * (uintptr_t)PMM_PAGE_SIZE;

		if (!hal_paging_query(virt, &phys, NULL)) continue;
		if (!hal_paging_unmap(virt)) continue;
		(void)pmm_free_pages(phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u), 1);
	}

	(void)vaddr_alloc_release(virt_base, reserved_pages);
}

bool vmm_init(void) {
	size_t heap_pages = KERNEL_HEAP_SIZE / PMM_PAGE_SIZE;

	spinlock_lock(&vmm_lock);
	initialized = false;

	if (!hal_paging_init()) {
		spinlock_unlock(&vmm_lock);
		return false;
	}
	if (!vaddr_alloc_init((uintptr_t)KERNEL_HEAP_BASE, heap_pages)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	initialized = true;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_is_initialized(void) {
	return initialized;
}

bool vmm_alloc_pages(size_t count, size_t align_pages, uint64_t flags, void** out_virt) {
	uintptr_t virt_base = 0;

	if (out_virt) *out_virt = NULL;

	if (!initialized || !out_virt || count == 0) return false;
	spinlock_lock(&vmm_lock);
	if (align_pages == 0) align_pages = 1;

	if (!vaddr_alloc_reserve(count, align_pages, &virt_base)) {
		spinlock_unlock(&vmm_lock);
		return false;
	}

	for (size_t page = 0; page < count; page++) {
		uintptr_t phys = 0;
		uintptr_t virt = virt_base + page * (uintptr_t)PMM_PAGE_SIZE;

		if (!pmm_alloc_pages(1, &phys)) {
			rollback_mapping(virt_base, page, count);
			spinlock_unlock(&vmm_lock);
			return false;
		}

		if (!hal_paging_map(virt, phys, flags)) {
			(void)pmm_free_pages(phys, 1);
			rollback_mapping(virt_base, page, count);
			spinlock_unlock(&vmm_lock);
			return false;
		}
	}

	*out_virt = (void*)virt_base;
	spinlock_unlock(&vmm_lock);
	return true;
}

bool vmm_free_pages(void* virt, size_t count) {
	uintptr_t virt_base = (uintptr_t)virt;

	if (!initialized || !virt || count == 0) return false;
	if ((virt_base & (PMM_PAGE_SIZE - 1u)) != 0) return false;
	spinlock_lock(&vmm_lock);

	for (size_t page = 0; page < count; page++) {
		uintptr_t phys = 0;
		uintptr_t addr = virt_base + page * (uintptr_t)PMM_PAGE_SIZE;

		if (!hal_paging_query(addr, &phys, NULL)) {
			spinlock_unlock(&vmm_lock);
			return false;
		}
	}

	for (size_t page = 0; page < count; page++) {
		uintptr_t phys = 0;
		uintptr_t addr = virt_base + page * (uintptr_t)PMM_PAGE_SIZE;

		if (!hal_paging_query(addr, &phys, NULL)) {
			spinlock_unlock(&vmm_lock);
			return false;
		}
		if (!hal_paging_unmap(addr)) {
			spinlock_unlock(&vmm_lock);
			return false;
		}
		if (!pmm_free_pages(phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1u), 1)) {
			spinlock_unlock(&vmm_lock);
			return false;
		}
	}

	bool ok = vaddr_alloc_release(virt_base, count);
	spinlock_unlock(&vmm_lock);
	return ok;
}

uintptr_t vmm_heap_base(void) {
	return (uintptr_t)KERNEL_HEAP_BASE;
}

size_t vmm_heap_page_count(void) {
	return KERNEL_HEAP_SIZE / PMM_PAGE_SIZE;
}
