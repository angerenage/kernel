#pragma once

#include <core/mm.h>
#include <core/pmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct paging_transaction_record {
	uint64_t* slot;
	uint64_t  previous;
	uintptr_t rollback_free_phys;
	uintptr_t commit_free_phys;
};

#define PAGING_TRANSACTION_INLINE_RECORDS 16u
#define PAGING_TRANSACTION_PAGE_RECORDS 96u

typedef void (*paging_transaction_restore_fn)(uint64_t* slot, uint64_t previous, void* context);

struct paging_transaction_page {
	uintptr_t                        previous_phys;
	size_t                           count;
	struct paging_transaction_record records[PAGING_TRANSACTION_PAGE_RECORDS];
};

_Static_assert(sizeof(struct paging_transaction_page) <= PMM_PAGE_SIZE,
               "paging transaction page must fit in one PMM page");

struct paging_transaction {
	struct paging_transaction_record inline_records[PAGING_TRANSACTION_INLINE_RECORDS];
	size_t                           inline_count;
	uintptr_t                        newest_page_phys;
	bool                             hierarchy_changed;
};

static inline struct paging_transaction_page* paging_transaction_page_virt(uintptr_t phys) {
	return (struct paging_transaction_page*)(phys + boot_info.direct_map_offset);
}

static inline bool paging_transaction_record_full(struct paging_transaction* transaction, uint64_t* slot,
                                                  uintptr_t rollback_free_phys, uintptr_t commit_free_phys) {
	if (transaction == NULL || slot == NULL) return false;
	struct paging_transaction_record record = {slot, *slot, rollback_free_phys, commit_free_phys};
	if (rollback_free_phys != 0u || commit_free_phys != 0u) transaction->hierarchy_changed = true;
	if (transaction->inline_count < PAGING_TRANSACTION_INLINE_RECORDS) {
		transaction->inline_records[transaction->inline_count++] = record;
		return true;
	}
	struct paging_transaction_page* page =
		transaction->newest_page_phys == 0u ? NULL : paging_transaction_page_virt(transaction->newest_page_phys);
	if (page == NULL || page->count == sizeof(page->records) / sizeof(page->records[0])) {
		uintptr_t page_phys = 0u;
		if (!pmm_alloc_pages(1u, &page_phys)) return false;
		page  = paging_transaction_page_virt(page_phys);
		*page = (struct paging_transaction_page){.previous_phys = transaction->newest_page_phys};
		transaction->newest_page_phys = page_phys;
	}
	page->records[page->count++] = record;
	return true;
}

static inline bool paging_transaction_record(struct paging_transaction* transaction, uint64_t* slot,
                                             uintptr_t allocated_table_phys) {
	return paging_transaction_record_full(transaction, slot, allocated_table_phys, 0u);
}

static inline bool paging_transaction_retire(struct paging_transaction* transaction, uint64_t* slot,
                                             uintptr_t retired_table_phys) {
	return paging_transaction_record_full(transaction, slot, 0u, retired_table_phys);
}

static inline void paging_transaction_release_pages(struct paging_transaction* transaction) {
	uintptr_t page_phys = transaction->newest_page_phys;
	while (page_phys != 0u) {
		struct paging_transaction_page* page          = paging_transaction_page_virt(page_phys);
		uintptr_t                       previous_phys = page->previous_phys;
		(void)pmm_free_pages(page_phys, 1u);
		page_phys = previous_phys;
	}
	transaction->newest_page_phys = 0u;
}

static inline void paging_transaction_rollback(struct paging_transaction*    transaction,
                                               paging_transaction_restore_fn restore, void* context) {
	uintptr_t page_phys = transaction->newest_page_phys;
	while (page_phys != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(page_phys);
		for (size_t i = page->count; i != 0u; i--) {
			struct paging_transaction_record* record = &page->records[i - 1u];
			restore(record->slot, record->previous, context);
		}
		page_phys = page->previous_phys;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--) {
		struct paging_transaction_record* record = &transaction->inline_records[i - 1u];
		restore(record->slot, record->previous, context);
	}
	/* The caller must invalidate translations before abort frees detached tables. */
}

static inline void paging_transaction_abort(struct paging_transaction* transaction) {
	uintptr_t page_phys = transaction->newest_page_phys;
	while (page_phys != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(page_phys);
		for (size_t i = page->count; i != 0u; i--) {
			if (page->records[i - 1u].rollback_free_phys != 0u)
				(void)pmm_free_pages(page->records[i - 1u].rollback_free_phys, 1u);
		}
		page_phys = page->previous_phys;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--) {
		if (transaction->inline_records[i - 1u].rollback_free_phys != 0u)
			(void)pmm_free_pages(transaction->inline_records[i - 1u].rollback_free_phys, 1u);
	}
	paging_transaction_release_pages(transaction);
	transaction->inline_count      = 0u;
	transaction->hierarchy_changed = false;
}

static inline void paging_transaction_commit(struct paging_transaction* transaction) {
	uintptr_t page_phys = transaction->newest_page_phys;
	while (page_phys != 0u) {
		struct paging_transaction_page* page = paging_transaction_page_virt(page_phys);
		for (size_t i = page->count; i != 0u; i--) {
			if (page->records[i - 1u].commit_free_phys != 0u)
				(void)pmm_free_pages(page->records[i - 1u].commit_free_phys, 1u);
		}
		page_phys = page->previous_phys;
	}
	for (size_t i = transaction->inline_count; i != 0u; i--) {
		if (transaction->inline_records[i - 1u].commit_free_phys != 0u)
			(void)pmm_free_pages(transaction->inline_records[i - 1u].commit_free_phys, 1u);
	}
	paging_transaction_release_pages(transaction);
	transaction->inline_count      = 0u;
	transaction->hierarchy_changed = false;
}
