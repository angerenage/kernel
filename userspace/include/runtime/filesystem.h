#pragma once

#include <base/syscall.h>
#include <protocol/filesystem.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A transport failure has FILESYSTEM_STATUS_NONE. Locally rejected arguments
 * report INVALID_ARGUMENT with SYSCALL_STATUS_BAD_ARGUMENT.
 */
struct filesystem_call_result {
	enum filesystem_status status;
	syscall_status_t       transport_status;
};

struct filesystem_node_handle {
	struct filesystem_node_info info;
	cap_id_t                    capability;
};

/* Install the borrowed VFS-directory capability extracted during startup. */
void filesystem_set_working_directory(cap_id_t directory_cap);

/* Return the borrowed VFS-directory capability, or CAP_ID_INVALID when unavailable. */
cap_id_t filesystem_get_working_directory(void);

/* Read metadata from a file capability. */
struct filesystem_call_result filesystem_file_info(cap_id_t file_cap, struct filesystem_node_info* out_info);

/* Read up to size bytes from a file at offset. */
struct filesystem_call_result filesystem_file_read(cap_id_t file_cap, uint64_t offset, void* buffer, size_t size,
                                                   size_t* out_read);

/* Write up to size bytes to a file at offset. */
struct filesystem_call_result filesystem_file_write(cap_id_t file_cap, uint64_t offset, const void* data, size_t size,
                                                    size_t* out_written);

/* Set the byte size of a file. */
struct filesystem_call_result filesystem_file_resize(cap_id_t file_cap, uint64_t size);

/* Read metadata from a raw-directory capability. */
struct filesystem_call_result filesystem_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info);

/* Open a normalized relative path below a raw directory. */
struct filesystem_call_result filesystem_directory_open(cap_id_t directory_cap, const void* path, size_t path_size,
                                                        cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Read up to count raw-directory entries starting at offset. */
struct filesystem_call_result filesystem_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                             struct filesystem_directory_entry* entries, size_t count,
                                                             size_t* out_returned, uint64_t* out_total);

/* Create and open a file at a normalized relative path. */
struct filesystem_call_result filesystem_directory_create_file(cap_id_t directory_cap, const void* path,
                                                               size_t path_size, cap_rights_t rights,
                                                               struct filesystem_node_handle* out_node);

/* Create and open a directory at a normalized relative path. */
struct filesystem_call_result filesystem_directory_create_directory(cap_id_t directory_cap, const void* path,
                                                                    size_t path_size, cap_rights_t rights,
                                                                    struct filesystem_node_handle* out_node);

/* Remove a file or empty directory at a normalized relative path. */
struct filesystem_call_result filesystem_directory_remove(cap_id_t directory_cap, const void* path, size_t path_size);

/* Atomically rename one normalized relative path to another. */
struct filesystem_call_result filesystem_directory_rename(cap_id_t directory_cap, const void* old_path,
                                                          size_t old_path_size, const void* new_path,
                                                          size_t new_path_size);

/* Read metadata of the node named by a normalized relative path without following a final symbolic link. */
struct filesystem_call_result filesystem_directory_lookup(cap_id_t directory_cap, const void* path, size_t path_size,
                                                          struct filesystem_node_info* out_info);

/* Create a symbolic link holding target at a normalized relative path. */
struct filesystem_call_result filesystem_directory_create_symbolic_link(cap_id_t directory_cap, const void* path,
                                                                        size_t path_size, const void* target,
                                                                        size_t target_size);

/* Read a link target into a buffer of at least FILESYSTEM_SYMBOLIC_LINK_MAX bytes. */
struct filesystem_call_result filesystem_directory_read_symbolic_link(cap_id_t directory_cap, const void* path,
                                                                      size_t path_size, void* target, size_t capacity,
                                                                      size_t* out_size);

/* Validate the exact byte sequence used by raw-directory path requests. */
bool filesystem_relative_path_valid(const void* path, size_t path_size);

/* Validate a symbolic-link target: 1 to FILESYSTEM_SYMBOLIC_LINK_MAX bytes without NUL. */
bool filesystem_symbolic_link_target_valid(const void* target, size_t target_size);
