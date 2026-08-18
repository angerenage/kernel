#pragma once

/*
 * Convenience helpers for discovering and using program loader services.
 *
 * These helpers are intentionally separate from the loader protocol itself.
 * They use init's service registry, cache the selected loader advertisement,
 * delegate Blob capabilities to its provider and perform the loader protocol
 * calls on behalf of the caller.
 */

#include <base/cap.h>
#include <base/syscall.h>
#include <stddef.h>

#define PROGRAM_LOADER_NAMESPACE "exec"

struct program_load_result {
	cap_id_t     load_cap;
	process_id_t process_id;
};

struct program_run_result {
	cap_id_t process_cap;
	cap_id_t thread_cap;
};

/* Resolve a loader service if necessary and prepare a program from blob_cap. */
syscall_status_t program_load(const char* service, cap_id_t blob_cap, const char* name, size_t name_size,
                              struct program_load_result* out_result);

/* Start a loading object and receive normal process and main-thread control. */
syscall_status_t program_run(cap_id_t load_cap, size_t argc, const char* const argv[],
                             struct program_run_result* out_result);

/* Abandon a loading object returned by program_load() without starting it. */
syscall_status_t program_cancel(cap_id_t load_cap);
