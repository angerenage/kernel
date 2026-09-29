#include <core/io_port.h>

#include "../mocks/hal/io_port_mock.h"
#include "../process/test_support.h"

Test(io_port, mappings_are_lazy_independent_and_generation_cached) {
	struct process*                  process = NULL;
	const struct hal_io_port_bitmap* bitmap;

	init_process_test_environment();
	hal_io_port_mock_reset();
	cr_assert_eq(process_create(&process, "io-port-state"), PROCESS_OK);
	cr_assert_not_null(process);
	cr_assert_null(process->io_port_state);
	cr_assert_eq(io_port_process_mapping_count(process), 0u);
	cr_assert_eq(io_port_process_generation(process), 0u);
	cr_assert_eq(io_port_map(process, CAP_ID_INVALID, 0x100u, 1u), IO_PORT_INVALID_ARGUMENTS);
	cr_assert_eq(io_port_map(process, 1u, HAL_IO_PORT_COUNT, 1u), IO_PORT_INVALID_ARGUMENTS);
	cr_assert_eq(io_port_map(process, 1u, 0x100u, 0u), IO_PORT_INVALID_ARGUMENTS);

	cr_assert_eq(io_port_map(process, 1u, 0x100u, 0x100u), IO_PORT_OK);
	cr_assert_not_null(process->io_port_state);
	cr_assert_eq(io_port_process_mapping_count(process), 1u);
	cr_assert_eq(io_port_process_generation(process), 1u);
	cr_assert_eq(io_port_map(process, 1u, 0x180u, 0x20u), IO_PORT_ALREADY_MAPPED);
	cr_assert_eq(io_port_map(process, 2u, 0x180u, 0x180u), IO_PORT_OK);
	cr_assert_eq(io_port_process_mapping_count(process), 2u);
	cr_assert_eq(io_port_process_generation(process), 2u);

	io_port_process_load(process);
	cr_assert_eq(hal_io_port_mock_load_count(), 1u);
	cr_assert(hal_io_port_mock_bitmap_present());
	bitmap = hal_io_port_mock_loaded_bitmap();
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x100u));
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x17fu));
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x180u));
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x2ffu));
	cr_assert(!hal_io_port_bitmap_port_allowed(bitmap, 0x300u));
	io_port_process_load(process);
	cr_assert_eq(hal_io_port_mock_load_count(), 1u);

	cr_assert_eq(io_port_unmap(process, 1u), IO_PORT_OK);
	cr_assert_eq(hal_io_port_mock_invalidation_count(), 1u);
	cr_assert_eq(io_port_process_mapping_count(process), 1u);
	cr_assert_eq(io_port_process_generation(process), 3u);
	io_port_process_load(process);
	cr_assert_eq(hal_io_port_mock_load_count(), 2u);
	bitmap = hal_io_port_mock_loaded_bitmap();
	cr_assert(!hal_io_port_bitmap_port_allowed(bitmap, 0x100u));
	cr_assert(!hal_io_port_bitmap_port_allowed(bitmap, 0x17fu));
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x180u));
	cr_assert(hal_io_port_bitmap_port_allowed(bitmap, 0x2ffu));

	cr_assert_eq(io_port_unmap(process, 2u), IO_PORT_OK);
	cr_assert_null(process->io_port_state);
	cr_assert_eq(io_port_process_generation(process), 0u);
	cr_assert_eq(hal_io_port_mock_invalidation_count(), 2u);
	io_port_process_load(process);
	cr_assert_eq(hal_io_port_mock_load_count(), 3u);
	cr_assert(!hal_io_port_mock_bitmap_present());
	cr_assert_eq(io_port_unmap(process, 2u), IO_PORT_NOT_MAPPED);
	cr_assert(process_destroy(process));
}

Test(io_port, teardown_revokes_all_mappings_once) {
	struct process* process = NULL;

	init_process_test_environment();
	hal_io_port_mock_reset();
	cr_assert_eq(process_create(&process, "io-port-teardown"), PROCESS_OK);
	cr_assert_eq(io_port_map(process, 10u, 0x3f8u, 8u), IO_PORT_OK);
	cr_assert_eq(io_port_map(process, 11u, 0x3f8u, 8u), IO_PORT_OK);
	cr_assert_eq(io_port_process_mapping_count(process), 2u);
	cr_assert(process_destroy(process));
	cr_assert_eq(hal_io_port_mock_invalidation_count(), 1u);
}
