#include "elf64.h"

#include <runtime/blob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define ELF_MAGIC0 0x7fu
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELF_CLASS_64 2u
#define ELF_DATA_LSB 1u
#define ELF_VERSION_CURRENT 1u

#define ELF_ET_EXEC 2u

#define ELF_PT_LOAD 1u
#define ELF_PT_DYNAMIC 2u
#define ELF_PT_INTERP 3u
#define ELF_PT_TLS 7u

#define ELF_PF_X 1u
#define ELF_PF_W 2u
#define ELF_PF_R 4u

#if defined(PLATFORM_PC_X86_64) || defined(__x86_64__)
#define ELF_TARGET_MACHINE 62u
#elif defined(PLATFORM_PC_AARCH64) || defined(__aarch64__)
#define ELF_TARGET_MACHINE 183u
#elif defined(PLATFORM_PC_RISCV64) || (defined(__riscv) && __riscv_xlen == 64)
#define ELF_TARGET_MACHINE 243u
#elif defined(PLATFORM_PC_LOONGARCH64) || defined(__loongarch64)
#define ELF_TARGET_MACHINE 258u
#else
#error "unsupported ELF64 loader target"
#endif

struct elf64_ehdr {
	uint8_t  ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
};

struct elf64_phdr {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

static bool add_u64(uint64_t left, uint64_t right, uint64_t* out) {
	if (out == NULL || left > UINT64_MAX - right) return false;
	*out = left + right;
	return true;
}

static bool mul_u64(uint64_t left, uint64_t right, uint64_t* out) {
	if (out == NULL || (right != 0u && left > UINT64_MAX / right)) return false;
	*out = left * right;
	return true;
}

static bool range_valid(uint64_t blob_size, uint64_t offset, uint64_t size) {
	uint64_t end;
	return add_u64(offset, size, &end) && end <= blob_size;
}

static bool read_exact(cap_id_t blob_cap, uint64_t offset, void* buffer, size_t size) {
	return blob_read(blob_cap, offset, buffer, size) == SYSCALL_STATUS_OK;
}

static bool power_of_two_u64(uint64_t value) {
	return value != 0u && (value & (value - 1u)) == 0u;
}

static enum elf64_parse_result validate_header(uint64_t blob_size, const struct elf64_ehdr* header) {
	uint64_t table_size;

	if (header->ident[0] != ELF_MAGIC0 || header->ident[1] != ELF_MAGIC1 || header->ident[2] != ELF_MAGIC2 ||
	    header->ident[3] != ELF_MAGIC3)
		return ELF64_PARSE_BAD_FORMAT;
	if (header->ident[4] != ELF_CLASS_64 || header->ident[5] != ELF_DATA_LSB || header->ident[6] != ELF_VERSION_CURRENT)
		return ELF64_PARSE_BAD_FORMAT;
	if (header->type != ELF_ET_EXEC) return ELF64_PARSE_UNSUPPORTED;
	if (header->machine != ELF_TARGET_MACHINE || header->version != ELF_VERSION_CURRENT) return ELF64_PARSE_BAD_FORMAT;
	if (header->ehsize != sizeof(*header) || header->phentsize != sizeof(struct elf64_phdr) || header->phnum == 0u)
		return ELF64_PARSE_BAD_FORMAT;
	if (!mul_u64((uint64_t)header->phentsize, (uint64_t)header->phnum, &table_size) ||
	    !range_valid(blob_size, header->phoff, table_size))
		return ELF64_PARSE_BAD_FORMAT;
	return ELF64_PARSE_OK;
}

static enum elf64_parse_result validate_load_segment(uint64_t blob_size, const struct elf64_phdr* phdr) {
	uint64_t memory_end;

	if (phdr->filesz > phdr->memsz) return ELF64_PARSE_BAD_FORMAT;
	if (!range_valid(blob_size, phdr->offset, phdr->filesz)) return ELF64_PARSE_BAD_FORMAT;
	if (!add_u64(phdr->vaddr, phdr->memsz, &memory_end)) return ELF64_PARSE_BAD_FORMAT;
	(void)memory_end;

	if (phdr->align > 1u) {
		if (!power_of_two_u64(phdr->align)) return ELF64_PARSE_BAD_FORMAT;
		if ((phdr->vaddr & (phdr->align - 1u)) != (phdr->offset & (phdr->align - 1u))) return ELF64_PARSE_BAD_FORMAT;
	}
	return ELF64_PARSE_OK;
}

static bool entry_in_segment(uint64_t entry, const struct elf64_phdr* phdr) {
	uint64_t end;
	if ((phdr->flags & ELF_PF_X) == 0u || phdr->memsz == 0u || !add_u64(phdr->vaddr, phdr->memsz, &end)) return false;
	return entry >= phdr->vaddr && entry < end;
}

void elf64_image_deinit(struct elf64_image* image) {
	if (image == NULL) return;
	free(image->segments);
	*image = (struct elf64_image){0};
}

enum elf64_parse_result elf64_image_parse(cap_id_t blob_cap, struct elf64_image* out_image) {
	struct blob_info_response info;
	struct elf64_ehdr         header;
	struct elf64_segment*     segments;
	size_t                    segment_count = 0u;
	bool                      entry_valid   = false;
	enum elf64_parse_result   result;

	if (out_image != NULL) *out_image = (struct elf64_image){0};
	if (blob_cap == CAP_ID_INVALID || out_image == NULL) return ELF64_PARSE_INVALID_ARGUMENT;
	if (blob_get_info(blob_cap, &info) != SYSCALL_STATUS_OK) return ELF64_PARSE_IO_ERROR;
	if (info.size < sizeof(header)) return ELF64_PARSE_BAD_FORMAT;
	if (!read_exact(blob_cap, 0u, &header, sizeof(header))) return ELF64_PARSE_IO_ERROR;
	result = validate_header(info.size, &header);
	if (result != ELF64_PARSE_OK) return result;

	segments = calloc((size_t)header.phnum, sizeof(*segments));
	if (segments == NULL) return ELF64_PARSE_NO_MEMORY;

	for (size_t i = 0u; i < (size_t)header.phnum; i++) {
		struct elf64_phdr phdr;
		uint64_t          offset;
		uint64_t          index_offset;

		if (!mul_u64((uint64_t)i, sizeof(phdr), &index_offset) || !add_u64(header.phoff, index_offset, &offset)) {
			result = ELF64_PARSE_BAD_FORMAT;
			goto fail;
		}
		if (!read_exact(blob_cap, offset, &phdr, sizeof(phdr))) {
			result = ELF64_PARSE_IO_ERROR;
			goto fail;
		}

		if (phdr.type == ELF_PT_INTERP || phdr.type == ELF_PT_DYNAMIC || phdr.type == ELF_PT_TLS) {
			result = ELF64_PARSE_UNSUPPORTED;
			goto fail;
		}
		if (phdr.type != ELF_PT_LOAD) continue;

		result = validate_load_segment(info.size, &phdr);
		if (result != ELF64_PARSE_OK) goto fail;
		if (entry_in_segment(header.entry, &phdr)) entry_valid = true;
		segments[segment_count++] = (struct elf64_segment){
			.offset = phdr.offset,
			.vaddr  = phdr.vaddr,
			.filesz = phdr.filesz,
			.memsz  = phdr.memsz,
			.align  = phdr.align,
			.flags  = ((phdr.flags & ELF_PF_X) != 0u ? ELF64_SEGMENT_EXEC : 0u) |
		             ((phdr.flags & ELF_PF_W) != 0u ? ELF64_SEGMENT_WRITE : 0u) |
		             ((phdr.flags & ELF_PF_R) != 0u ? ELF64_SEGMENT_READ : 0u),
		};
	}

	if (segment_count == 0u || !entry_valid) {
		result = ELF64_PARSE_BAD_FORMAT;
		goto fail;
	}

	*out_image = (struct elf64_image){
		.entry         = header.entry,
		.segments      = segments,
		.segment_count = segment_count,
	};
	return ELF64_PARSE_OK;

fail:
	free(segments);
	return result;
}
