#pragma once

#include <base/io_port.h>
#include <stddef.h>
#include <stdint.h>

void               io_port_instruction_mock_reset(void);
void               io_port_instruction_mock_set_read_value(uint32_t value);
size_t             io_port_instruction_mock_read_count(void);
size_t             io_port_instruction_mock_write_count(void);
uint16_t           io_port_instruction_mock_last_port(void);
enum io_port_width io_port_instruction_mock_last_width(void);
uint32_t           io_port_instruction_mock_last_write_value(void);
