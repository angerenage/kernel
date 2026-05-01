#include <core/id_table.h>
#include <criterion/criterion.h>
#include <stddef.h>
#include <stdint.h>

#include "../kheap/kheap_test.h"
#include "../thread_test.h"

#define ID_TABLE_TEST_HEAP_SIZE KiB(256)
#define ID_TABLE_CONCURRENT_THREADS 4u
#define ID_TABLE_CONCURRENT_ALLOCS 128u

static void init_id_table_test_heap(void) {
	_Alignas(4096) static uint8_t arena[ID_TABLE_TEST_HEAP_SIZE];

	init_test_kheap(arena, sizeof(arena));
}

Test(id_table, allocates_sequential_ids_and_supports_lookup) {
	struct id_table table;
	int             first_object  = 11;
	int             second_object = 22;
	id_table_id_t   first_id      = ID_TABLE_ID_INVALID;
	id_table_id_t   second_id     = ID_TABLE_ID_INVALID;

	init_id_table_test_heap();

	cr_assert(id_table_init(&table, "id_table_test", 1u, 64u), "id_table_init failed");
	cr_assert(id_table_alloc(&table, &first_object, &first_id), "first allocation failed");
	cr_assert(id_table_alloc(&table, &second_object, &second_id), "second allocation failed");

	cr_assert_eq(first_id, 1u, "first ID should start at min_id");
	cr_assert_eq(second_id, 2u, "second ID should be sequential");
	cr_assert_eq(id_table_lookup(&table, first_id), &first_object, "first lookup returned wrong object");
	cr_assert_eq(id_table_lookup(&table, second_id), &second_object, "second lookup returned wrong object");
	cr_assert_eq(id_table_count(&table), 2u, "table count mismatch");

	id_table_deinit(&table);
}

Test(id_table, remove_clears_slot_and_returns_object) {
	struct id_table table;
	int             object = 33;
	void*           removed;
	id_table_id_t   id = ID_TABLE_ID_INVALID;

	init_id_table_test_heap();

	cr_assert(id_table_init(&table, "id_table_test", 1u, 16u), "id_table_init failed");
	cr_assert(id_table_alloc(&table, &object, &id), "allocation failed");

	removed = NULL;
	cr_assert(id_table_remove(&table, id, &removed), "remove should accept a present ID");
	cr_assert_eq(removed, &object, "remove returned wrong object");
	cr_assert_null(id_table_lookup(&table, id), "removed ID should not be visible");
	cr_assert_eq(id_table_count(&table), 0u, "count should decrease after remove");
	cr_assert(!id_table_remove(&table, id, NULL), "removing an absent ID should fail");

	id_table_deinit(&table);
}

Test(id_table, rejects_invalid_arguments_and_exhaustion) {
	struct id_table table;
	int             first_object  = 44;
	int             second_object = 55;
	id_table_id_t   id            = ID_TABLE_ID_INVALID;

	init_id_table_test_heap();

	cr_assert(!id_table_init(NULL, "id_table_test", 1u, 16u), "NULL table should be rejected");
	cr_assert(!id_table_init(&table, "id_table_test", ID_TABLE_ID_INVALID, 16u), "zero min ID should be rejected");
	cr_assert(!id_table_init(&table, "id_table_test", 4u, 3u), "inverted range should be rejected");

	cr_assert(id_table_init(&table, "id_table_test", 7u, 7u), "single-ID table should initialize");
	cr_assert(!id_table_alloc(&table, NULL, &id), "NULL object should be rejected");
	cr_assert(!id_table_alloc(&table, &first_object, NULL), "NULL output ID should be rejected");
	cr_assert(id_table_alloc(&table, &first_object, &id), "first allocation should consume the only ID");
	cr_assert_eq(id, 7u, "single-ID table returned wrong ID");
	cr_assert(!id_table_alloc(&table, &second_object, &id), "exhausted table should reject allocation");
	cr_assert_eq(id, ID_TABLE_ID_INVALID, "failed allocation should clear output ID");
	cr_assert_null(id_table_lookup(&table, ID_TABLE_ID_INVALID), "invalid ID should not resolve");
	cr_assert(!id_table_remove(&table, 8u, NULL), "out-of-range remove should fail");

	id_table_deinit(&table);
}

Test(id_table, grows_slot_storage_without_reusing_removed_ids) {
	struct id_table table;
	int             objects[40];
	id_table_id_t   ids[40];
	void*           removed;

	init_id_table_test_heap();

	cr_assert(id_table_init(&table, "id_table_test", 1u, 128u), "id_table_init failed");
	for (size_t i = 0; i < 40u; i++) {
		objects[i] = (int)i;
		ids[i]     = ID_TABLE_ID_INVALID;
		cr_assert(id_table_alloc(&table, &objects[i], &ids[i]), "allocation %zu failed", i);
		cr_assert_eq(ids[i], i + 1u, "allocation %zu returned unexpected ID", i);
	}

	cr_assert_eq(id_table_lookup(&table, ids[39]), &objects[39], "lookup after growth failed");
	removed = NULL;
	cr_assert(id_table_remove(&table, ids[3], &removed), "remove before next allocation failed");
	cr_assert_eq(removed, &objects[3], "remove returned wrong object");
	cr_assert(id_table_alloc(&table, &objects[3], &ids[3]), "allocation after remove failed");
	cr_assert_eq(ids[3], 41u, "removed ID should not be reused in the simple table");
	cr_assert_eq(id_table_count(&table), 40u, "count should track live objects after remove and alloc");

	id_table_deinit(&table);
}

struct id_table_concurrent_ctx {
	struct id_table*     table;
	struct test_barrier* barrier;
	id_table_id_t*       ids;
	int                  object;
	bool                 ok;
};

static void* id_table_concurrent_alloc_worker(void* arg) {
	struct id_table_concurrent_ctx* ctx = (struct id_table_concurrent_ctx*)arg;

	test_barrier_wait(ctx->barrier);
	for (size_t i = 0; i < ID_TABLE_CONCURRENT_ALLOCS; i++) {
		if (!id_table_alloc(ctx->table, &ctx->object, &ctx->ids[i])) {
			ctx->ok = false;
			return NULL;
		}
	}

	return NULL;
}

Test(id_table, concurrent_allocations_get_unique_ids) {
	struct id_table                table;
	struct test_barrier            barrier;
	pthread_t                      threads[ID_TABLE_CONCURRENT_THREADS];
	struct id_table_concurrent_ctx ctx[ID_TABLE_CONCURRENT_THREADS];
	id_table_id_t                  ids[ID_TABLE_CONCURRENT_THREADS][ID_TABLE_CONCURRENT_ALLOCS]        = {{0}};
	bool                           seen[ID_TABLE_CONCURRENT_THREADS * ID_TABLE_CONCURRENT_ALLOCS + 1u] = {false};

	init_id_table_test_heap();

	cr_assert(id_table_init(&table, "id_table_test", 1u, ID_TABLE_CONCURRENT_THREADS * ID_TABLE_CONCURRENT_ALLOCS),
	          "id_table_init failed");
	test_barrier_init(&barrier, ID_TABLE_CONCURRENT_THREADS);

	for (size_t i = 0; i < ID_TABLE_CONCURRENT_THREADS; i++) {
		ctx[i] = (struct id_table_concurrent_ctx){
			.table   = &table,
			.barrier = &barrier,
			.ids     = ids[i],
			.object  = (int)i,
			.ok      = true,
		};
		cr_assert_eq(
			pthread_create(&threads[i], NULL, id_table_concurrent_alloc_worker, &ctx[i]), 0, "pthread_create failed");
	}

	for (size_t i = 0; i < ID_TABLE_CONCURRENT_THREADS; i++) {
		cr_assert_eq(pthread_join(threads[i], NULL), 0, "pthread_join failed");
		cr_assert(ctx[i].ok, "worker %zu reported allocation failure", i);
	}

	for (size_t i = 0; i < ID_TABLE_CONCURRENT_THREADS; i++) {
		for (size_t j = 0; j < ID_TABLE_CONCURRENT_ALLOCS; j++) {
			id_table_id_t id = ids[i][j];

			cr_assert_geq(id, 1u, "worker %zu allocation %zu returned invalid low ID", i, j);
			cr_assert_leq(id,
			              ID_TABLE_CONCURRENT_THREADS * ID_TABLE_CONCURRENT_ALLOCS,
			              "worker %zu allocation %zu returned out-of-range ID",
			              i,
			              j);
			cr_assert(!seen[(size_t)id], "duplicate ID %llu", (unsigned long long)id);
			seen[(size_t)id] = true;
		}
	}
	cr_assert_eq(id_table_count(&table), ID_TABLE_CONCURRENT_THREADS * ID_TABLE_CONCURRENT_ALLOCS);

	id_table_deinit(&table);
}
