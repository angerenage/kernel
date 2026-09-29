#pragma once

#include <hal/io_port.h>
#include <stdbool.h>
#include <stddef.h>

void                             hal_io_port_mock_reset(void);
const struct hal_io_port_bitmap* hal_io_port_mock_loaded_bitmap(void);
bool                             hal_io_port_mock_bitmap_present(void);
size_t                           hal_io_port_mock_load_count(void);
size_t                           hal_io_port_mock_invalidation_count(void);
