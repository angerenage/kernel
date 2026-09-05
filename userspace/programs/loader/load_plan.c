#include "load_plan.h"

#include <base/math.h>
#include <base/vmm.h>
#include <stdlib.h>

static void sort_boundaries(uint64_t* values, size_t count) {
	for (size_t i = 1u; i < count; i++) {
		uint64_t value = values[i];
		size_t   j     = i;
		while (j != 0u && values[j - 1u] > value) {
			values[j] = values[j - 1u];
			j--;
		}
		values[j] = value;
	}
}

static bool logical_ranges_overlap(const struct loader_elf_segment_layout* left,
                                   const struct loader_elf_segment_layout* right) {
	uint64_t left_end;
	uint64_t right_end;
	if (left->memsz == 0u || right->memsz == 0u) return false;
	if (add_overflow_u64(left->vaddr, left->memsz, &left_end) ||
	    add_overflow_u64(right->vaddr, right->memsz, &right_end))
		return true;
	return left->vaddr < right_end && right->vaddr < left_end;
}

void loader_elf_plan_deinit(struct loader_elf_load_plan* plan) {
	if (plan == NULL) return;
	free(plan->runs);
	free(plan->regions);
	*plan = (struct loader_elf_load_plan){0};
}

bool loader_elf_entry_is_executable(const struct loader_elf_segment_layout* segments, size_t segment_count,
                                    uint64_t entry) {
	if (segments == NULL) return false;
	for (size_t i = 0u; i < segment_count; i++) {
		uint64_t end;
		if (segments[i].memsz == 0u || (segments[i].prot & VMM_PROT_EXEC) == 0u ||
		    add_overflow_u64(segments[i].vaddr, segments[i].memsz, &end))
			continue;
		if (entry >= segments[i].vaddr && entry < end) return true;
	}
	return false;
}

enum loader_elf_plan_result loader_elf_plan_create(const struct loader_elf_segment_layout* segments,
                                                   size_t segment_count, struct loader_elf_load_plan* out_plan) {
	uint64_t* boundaries;
	size_t    boundary_count = 0u;
	size_t    boundary_capacity;
	if (out_plan == NULL || (segments == NULL && segment_count != 0u)) return LOADER_ELF_PLAN_INVALID_ARGUMENT;
	*out_plan = (struct loader_elf_load_plan){0};
	if (segment_count > SIZE_MAX / 2u) return LOADER_ELF_PLAN_BAD_LAYOUT;
	boundary_capacity = segment_count * 2u;
	if (boundary_capacity == 0u) return LOADER_ELF_PLAN_OK;
	boundaries = calloc(boundary_capacity, sizeof(*boundaries));
	if (boundaries == NULL) return LOADER_ELF_PLAN_NO_MEMORY;

	for (size_t i = 0u; i < segment_count; i++) {
		uint64_t logical_end;
		uint64_t page_end;
		if (segments[i].memsz == 0u) continue;
		if (add_overflow_u64(segments[i].vaddr, segments[i].memsz, &logical_end) ||
		    !align_up_u64(logical_end, VMM_PAGE_SIZE, &page_end))
			goto bad_layout;
		for (size_t j = 0u; j < i; j++)
			if (logical_ranges_overlap(&segments[i], &segments[j])) goto bad_layout;
		boundaries[boundary_count++] = align_down_u64(segments[i].vaddr, VMM_PAGE_SIZE);
		boundaries[boundary_count++] = page_end;
	}
	if (boundary_count == 0u) {
		free(boundaries);
		return LOADER_ELF_PLAN_OK;
	}
	sort_boundaries(boundaries, boundary_count);
	size_t unique_count = 1u;
	for (size_t i = 1u; i < boundary_count; i++)
		if (boundaries[i] != boundaries[unique_count - 1u]) boundaries[unique_count++] = boundaries[i];
	out_plan->regions = calloc(segment_count, sizeof(*out_plan->regions));
	out_plan->runs    = calloc(unique_count - 1u, sizeof(*out_plan->runs));
	if (out_plan->regions == NULL || out_plan->runs == NULL) {
		free(boundaries);
		loader_elf_plan_deinit(out_plan);
		return LOADER_ELF_PLAN_NO_MEMORY;
	}

	bool covered_before = false;
	for (size_t i = 0u; i + 1u < unique_count; i++) {
		uint64_t   start   = boundaries[i];
		uint64_t   end     = boundaries[i + 1u];
		vmm_prot_t prot    = VMM_PROT_NONE;
		bool       covered = false;
		for (size_t j = 0u; j < segment_count; j++) {
			uint64_t logical_end;
			uint64_t page_end;
			if (segments[j].memsz == 0u || add_overflow_u64(segments[j].vaddr, segments[j].memsz, &logical_end) ||
			    !align_up_u64(logical_end, VMM_PAGE_SIZE, &page_end))
				continue;
			uint64_t page_start = align_down_u64(segments[j].vaddr, VMM_PAGE_SIZE);
			if (page_start <= start && page_end >= end) {
				covered = true;
				prot |= segments[j].prot;
			}
		}
		if (!covered) {
			covered_before = false;
			continue;
		}
		if (!covered_before) {
			struct loader_elf_load_region* region = &out_plan->regions[out_plan->region_count++];
			region->virtual_base                  = start;
			region->first_run                     = out_plan->run_count;
		}
		struct loader_elf_load_region* region = &out_plan->regions[out_plan->region_count - 1u];
		if ((end - region->virtual_base) / VMM_PAGE_SIZE > SIZE_MAX) goto bad_plan;
		region->page_count = (size_t)((end - region->virtual_base) / VMM_PAGE_SIZE);
		if (region->run_count != 0u) {
			struct loader_elf_load_run* previous = &out_plan->runs[out_plan->run_count - 1u];
			size_t                      offset   = (size_t)((start - region->virtual_base) / VMM_PAGE_SIZE);
			if (previous->prot == prot && previous->object_page_offset + previous->page_count == offset) {
				previous->page_count += (size_t)((end - start) / VMM_PAGE_SIZE);
				covered_before = true;
				continue;
			}
		}
		struct loader_elf_load_run* run = &out_plan->runs[out_plan->run_count++];
		run->object_page_offset         = (size_t)((start - region->virtual_base) / VMM_PAGE_SIZE);
		run->page_count                 = (size_t)((end - start) / VMM_PAGE_SIZE);
		run->prot                       = prot;
		region->run_count++;
		covered_before = true;
	}
	free(boundaries);
	return LOADER_ELF_PLAN_OK;

bad_plan:
	free(boundaries);
	loader_elf_plan_deinit(out_plan);
	return LOADER_ELF_PLAN_BAD_LAYOUT;
bad_layout:
	free(boundaries);
	return LOADER_ELF_PLAN_BAD_LAYOUT;
}
