#pragma once

#include <base/cap.h>
#include <stddef.h>
#include <stdint.h>

enum elf64_parse_result {
	ELF64_PARSE_OK = 0,
	ELF64_PARSE_INVALID_ARGUMENT,
	ELF64_PARSE_IO_ERROR,
	ELF64_PARSE_BAD_FORMAT,
	ELF64_PARSE_UNSUPPORTED,
	ELF64_PARSE_NO_MEMORY,
};

enum elf64_segment_flag {
	ELF64_SEGMENT_EXEC  = 1u << 0,
	ELF64_SEGMENT_WRITE = 1u << 1,
	ELF64_SEGMENT_READ  = 1u << 2,
};

struct elf64_segment {
	uint64_t offset;
	uint64_t vaddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
	uint32_t flags;
};

struct elf64_image {
	uint64_t              entry;
	struct elf64_segment* segments;
	size_t                segment_count;
};

/* Parse and validate a static ELF64 executable exposed by a Blob capability. */
enum elf64_parse_result elf64_image_parse(cap_id_t blob_cap, struct elf64_image* out_image);

/* Release resources owned by an image returned by elf64_image_parse(). */
void elf64_image_deinit(struct elf64_image* image);
