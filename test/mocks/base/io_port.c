#include "io_port_mock.h"

static uint32_t           read_value;
static size_t             read_count;
static size_t             write_count;
static uint16_t           last_port;
static enum io_port_width last_width;
static uint32_t           last_write_value;

uint8_t io_read8(uint16_t port) {
	read_count++;
	last_port  = port;
	last_width = IO_PORT_WIDTH_8;
	return (uint8_t)read_value;
}

uint16_t io_read16(uint16_t port) {
	read_count++;
	last_port  = port;
	last_width = IO_PORT_WIDTH_16;
	return (uint16_t)read_value;
}

uint32_t io_read32(uint16_t port) {
	read_count++;
	last_port  = port;
	last_width = IO_PORT_WIDTH_32;
	return read_value;
}

void io_write8(uint16_t port, uint8_t value) {
	write_count++;
	last_port        = port;
	last_width       = IO_PORT_WIDTH_8;
	last_write_value = value;
}

void io_write16(uint16_t port, uint16_t value) {
	write_count++;
	last_port        = port;
	last_width       = IO_PORT_WIDTH_16;
	last_write_value = value;
}

void io_write32(uint16_t port, uint32_t value) {
	write_count++;
	last_port        = port;
	last_width       = IO_PORT_WIDTH_32;
	last_write_value = value;
}

void io_port_instruction_mock_reset(void) {
	read_value       = 0u;
	read_count       = 0u;
	write_count      = 0u;
	last_port        = 0u;
	last_width       = 0u;
	last_write_value = 0u;
}

void io_port_instruction_mock_set_read_value(uint32_t value) {
	read_value = value;
}

size_t io_port_instruction_mock_read_count(void) {
	return read_count;
}

size_t io_port_instruction_mock_write_count(void) {
	return write_count;
}

uint16_t io_port_instruction_mock_last_port(void) {
	return last_port;
}

enum io_port_width io_port_instruction_mock_last_width(void) {
	return last_width;
}

uint32_t io_port_instruction_mock_last_write_value(void) {
	return last_write_value;
}
