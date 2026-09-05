#pragma once

#include <core/mm.h>
#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct paging_transaction_record {
	uint64_t*         slot;
	uint64_t          previous;
	struct pmm_extent rollback_free;
	struct pmm_extent commit_free;
};

#define PAGING_TRANSACTION_INLINE_RECORDS 16u

typedef void (*paging_transaction_restore_fn)(uint64_t* slot, uint64_t previous, void* context);

struct paging_transaction_page {
	struct pmm_extent                previous;
	size_t                           count;
	struct paging_transaction_record records[];
};

struct paging_transaction {
	struct paging_transaction_record inline_records[PAGING_TRANSACTION_INLINE_RECORDS];
	size_t                           inline_count;
	struct pmm_extent                newest_page;
	bool                             hierarchy_changed;
};

static inline struct paging_transaction_page* paging_transaction_page_virt(uintptr_t phys) {
	return (struct paging_transaction_page*)(phys + boot_info.direct_map_offset);
}

static inline size_t paging_transaction_page_capacity(void) {
	const struct pmm_info* info = pmm_info();
	if (info == NULL || info->allocation_granule < sizeof(struct paging_transaction_page)) return 0u;
	return (info->allocation_granule - sizeof(struct paging_transaction_page)) /
	       sizeof(struct paging_transaction_record);
}

static inline bool paging_transaction_record_full(struct paging_transaction* transaction, uint64_t* slot,
                                                  struct pmm_extent rollback_free, struct pmm_extent commit_free) {
	if (transaction == NULL || slot == NULL) return false;
	struct paging_transaction_record record = {slot, *slot, rollback_free, commit_free};
	if (rollback_free.size != 0u || commit_free.size != 0u) transaction->hierarchy_changed = true;
	if (transaction->inline_count < PAGING_TRANSACTION_INLINE_RECORDS) {
		transaction->inline_records[transaction->inline_count++] = record;
		return true;
	}

	size_t capacity = paging_transaction_page_capacity();
	if (capacity == 0u) return false;
	struct paging_transaction_page* page =
		transaction->newest_page.size == 0u ? NULL : paging_transaction_page_virt(transaction->newest_page.address);
	if (page == NULL || page->count == capacity) {
		const struct pmm_info* info = pmm_info();
		struct pmm_extent      allocation;
		if (!pmm_alloc(&(const struct pmm_alloc_request){.size = info->allocation_granule}, &allocation)) return false;
		page                     = paging_transaction_page_virt(allocation.address);
		page->previous           = transaction->newest_page;
		page->count              = 0u;
		transaction->newest_page = allocation;
	}
	page->records[page->count++] = record;
	return true;
}

static inline bool paging_transaction_record(struct paging_transaction* transaction, uint64_t* slot,
                                             struct pmm_extent allocated_table) {
	return paging_transaction_record_full(transaction, slot, allocated_table, (struct pmm_extent){0});
}

static inline bool paging_transaction_retire(struct paging_transaction* transaction, uint64_t* slot,
                                             struct pmm_extent retired_table) {
	return paging_transaction_record_full(transaction, slot, (struct pmm_extent){0}, retired_table);
}

static inline void paging_transaction_release_pages(struct paging_transaction* transaction) {
	struct pmm_extent allocation = transaction->newest_page;
	while (allocation.size != 0u) {
		struct paging_transaction_page* page     = paging_transaction_page_virt(allocation.address);
		struct pmm_extent               previous = page->previous;
		(void)pmm_free(allocation);
		allocation = previous;
	}
	transaction->newest_page = (struct pmm_extent){0};
}

static inline void paging_transaction_rollback(struct paging_transaction*    transaction,
                                               paging_transaction_restore_fn restore, void* context) {
	struct pmm_extent allocation = transaction->newest_page;
	while (allocation.size != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(allocation.address);
		for (size_t i = page->count; i != 0u; i--) {
			struct paging_transaction_record* record = &page->records[i - 1u];
			restore(record->slot, record->previous, context);
		}
		allocation = page->previous;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--) {
		struct paging_transaction_record* record = &transaction->inline_records[i - 1u];
		restore(record->slot, record->previous, context);
	}
	/* The caller must invalidate translations before abort frees detached tables. */
}

static inline void paging_transaction_abort(struct paging_transaction* transaction) {
	struct pmm_extent allocation = transaction->newest_page;
	while (allocation.size != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(allocation.address);
		for (size_t i = page->count; i != 0u; i--)
			if (page->records[i - 1u].rollback_free.size != 0u) (void)pmm_free(page->records[i - 1u].rollback_free);
		allocation = page->previous;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--)
		if (transaction->inline_records[i - 1u].rollback_free.size != 0u)
			(void)pmm_free(transaction->inline_records[i - 1u].rollback_free);
	paging_transaction_release_pages(transaction);
	transaction->inline_count      = 0u;
	transaction->hierarchy_changed = false;
}

static inline void paging_transaction_commit(struct paging_transaction* transaction) {
	struct pmm_extent allocation = transaction->newest_page;
	while (allocation.size != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(allocation.address);
		for (size_t i = page->count; i != 0u; i--)
			if (page->records[i - 1u].commit_free.size != 0u) (void)pmm_free(page->records[i - 1u].commit_free);
		allocation = page->previous;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--)
		if (transaction->inline_records[i - 1u].commit_free.size != 0u)
			(void)pmm_free(transaction->inline_records[i - 1u].commit_free);
	paging_transaction_release_pages(transaction);
	transaction->inline_count      = 0u;
	transaction->hierarchy_changed = false;
}
