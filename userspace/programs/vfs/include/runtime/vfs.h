#pragma once

#include <runtime/filesystem.h>

/* Open an absolute path in the global namespace. */
struct filesystem_call_result vfs_open(cap_id_t vfs_cap, const void* path, size_t path_size, cap_rights_t rights,
                                       struct filesystem_node_handle* out_node);

/* Create and open a file at an absolute path. */
struct filesystem_call_result vfs_create_file(cap_id_t vfs_cap, const void* path, size_t path_size, cap_rights_t rights,
                                              struct filesystem_node_handle* out_node);

/* Create and open a directory at an absolute path. */
struct filesystem_call_result vfs_create_directory(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                   cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Remove a file or empty directory at an absolute path. */
struct filesystem_call_result vfs_remove(cap_id_t vfs_cap, const void* path, size_t path_size);

/* Rename one absolute path to another. */
struct filesystem_call_result vfs_rename(cap_id_t vfs_cap, const void* old_path, size_t old_path_size,
                                         const void* new_path, size_t new_path_size);

/* Read metadata from a VFS-directory capability. */
struct filesystem_call_result vfs_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info);

/* Read up to count VFS-directory entries starting at offset. */
struct filesystem_call_result vfs_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                      struct filesystem_directory_entry* entries, size_t count,
                                                      size_t* out_returned, uint64_t* out_total);

/* Mount a delegated raw root on a VFS directory. */
struct filesystem_call_result vfs_directory_mount(cap_id_t directory_cap, cap_id_t fs_root_cap, uint32_t flags);

/* Unmount the raw filesystem covering a VFS directory. */
struct filesystem_call_result vfs_directory_unmount(cap_id_t directory_cap);

/* Validate the exact byte sequence used by VFS namespace requests. */
bool vfs_absolute_path_valid(const void* path, size_t path_size);
