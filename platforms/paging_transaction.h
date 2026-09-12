#pragma once

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

bool   paging_transaction_empty(const struct paging_transaction* transaction);
size_t paging_transaction_page_capacity(void);
bool   paging_transaction_record(struct paging_transaction* transaction, uint64_t* slot,
                                 struct pmm_extent allocated_table);
bool paging_transaction_retire(struct paging_transaction* transaction, uint64_t* slot, struct pmm_extent retired_table);
void paging_transaction_rollback(struct paging_transaction* transaction, paging_transaction_restore_fn restore,
                                 void* context);
void paging_transaction_abort(struct paging_transaction* transaction);
void paging_transaction_commit(struct paging_transaction* transaction);
