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

/* Mount a delegated raw root at an existing or synthetic absolute directory. */
struct filesystem_call_result vfs_directory_mount(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                  cap_id_t fs_root_cap, uint32_t flags);

/* Unmount the raw filesystem covering exactly one absolute path. */
struct filesystem_call_result vfs_directory_unmount(cap_id_t vfs_cap, const void* path, size_t path_size);

/* Read metadata from a VFS-directory capability. */
struct filesystem_call_result vfs_directory_info(cap_id_t directory_cap, struct filesystem_node_info* out_info);

/* Open a relative path from a VFS directory. */
struct filesystem_call_result vfs_directory_open(cap_id_t directory_cap, const void* path, size_t path_size,
                                                 cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Create and open a file at a path relative to a VFS directory. */
struct filesystem_call_result vfs_directory_create_file(cap_id_t directory_cap, const void* path, size_t path_size,
                                                        cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Create and open a directory at a path relative to a VFS directory. */
struct filesystem_call_result vfs_directory_create_directory(cap_id_t directory_cap, const void* path, size_t path_size,
                                                             cap_rights_t                   rights,
                                                             struct filesystem_node_handle* out_node);

/* Read up to count VFS-directory entries starting at offset. */
struct filesystem_call_result vfs_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                      struct filesystem_directory_entry* entries, size_t count,
                                                      size_t* out_returned, uint64_t* out_total);

/* Remove a path relative to a VFS directory. */
struct filesystem_call_result vfs_directory_remove(cap_id_t directory_cap, const void* path, size_t path_size);

/* Remove a path while deferring backing-device synchronization until SYNC. */
struct filesystem_call_result vfs_directory_remove_deferred(cap_id_t directory_cap, const void* path, size_t path_size);

/* Commit deferred mutations on the filesystem backing this directory. */
struct filesystem_call_result vfs_directory_sync(cap_id_t directory_cap);

/* Rename between paths relative to one VFS directory. */
struct filesystem_call_result vfs_directory_rename(cap_id_t directory_cap, const void* old_path, size_t old_path_size,
                                                   const void* new_path, size_t new_path_size);

/* Validate the exact byte sequence used by VFS namespace requests. */
bool vfs_absolute_path_valid(const void* path, size_t path_size);

/* Validate a relative VFS path. */
bool vfs_relative_path_valid(const void* path, size_t path_size);

/* Resolve a relative VFS path against a normalized absolute directory path. */
bool vfs_resolve_directory_path(const void* directory, size_t directory_size, const void* relative,
                                size_t relative_size, void* output, size_t output_capacity, size_t* out_size);
