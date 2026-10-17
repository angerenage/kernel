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

/* Open a normalized relative path below a VFS directory. */
struct filesystem_call_result vfs_directory_open(cap_id_t directory_cap, const void* path, size_t path_size,
                                                 cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Create and open a file at a normalized path below a VFS directory. */
struct filesystem_call_result vfs_directory_create_file(cap_id_t directory_cap, const void* path, size_t path_size,
                                                        cap_rights_t rights, struct filesystem_node_handle* out_node);

/* Create and open a directory at a normalized path below a VFS directory. */
struct filesystem_call_result vfs_directory_create_directory(cap_id_t directory_cap, const void* path, size_t path_size,
                                                             cap_rights_t                   rights,
                                                             struct filesystem_node_handle* out_node);

/* Read up to count VFS-directory entries starting at offset. */
struct filesystem_call_result vfs_directory_enumerate(cap_id_t directory_cap, uint64_t offset,
                                                      struct filesystem_directory_entry* entries, size_t count,
                                                      size_t* out_returned, uint64_t* out_total);

/* Remove a normalized relative path below a VFS directory. */
struct filesystem_call_result vfs_directory_remove(cap_id_t directory_cap, const void* path, size_t path_size);

/* Rename between normalized relative paths below one VFS directory. */
struct filesystem_call_result vfs_directory_rename(cap_id_t directory_cap, const void* old_path, size_t old_path_size,
                                                   const void* new_path, size_t new_path_size);

/* Read metadata of an absolute path without following a final symbolic link. */
struct filesystem_call_result vfs_lookup(cap_id_t vfs_cap, const void* path, size_t path_size,
                                         struct filesystem_node_info* out_info);

/* Create a symbolic link holding target at an absolute path. */
struct filesystem_call_result vfs_create_symbolic_link(cap_id_t vfs_cap, const void* path, size_t path_size,
                                                       const void* target, size_t target_size);

/* Read a link target into a buffer of at least FILESYSTEM_SYMBOLIC_LINK_MAX bytes. */
struct filesystem_call_result vfs_read_symbolic_link(cap_id_t vfs_cap, const void* path, size_t path_size, void* target,
                                                     size_t capacity, size_t* out_size);

/* Follow every symbolic link of an existing absolute path into a buffer of at least VFS_PATH_MAX bytes. */
struct filesystem_call_result vfs_resolve(cap_id_t vfs_cap, const void* path, size_t path_size, char* resolved,
                                          size_t capacity, size_t* out_size);

/* Read metadata of a normalized relative path below a VFS directory without following a final symbolic link. */
struct filesystem_call_result vfs_directory_lookup(cap_id_t directory_cap, const void* path, size_t path_size,
                                                   struct filesystem_node_info* out_info);

/* Create a symbolic link holding target at a normalized relative path below a VFS directory. */
struct filesystem_call_result vfs_directory_create_symbolic_link(cap_id_t directory_cap, const void* path,
                                                                 size_t path_size, const void* target,
                                                                 size_t target_size);

/* Read the target of a link below a VFS directory into a buffer of at least FILESYSTEM_SYMBOLIC_LINK_MAX bytes. */
struct filesystem_call_result vfs_directory_read_symbolic_link(cap_id_t directory_cap, const void* path,
                                                               size_t path_size, void* target, size_t capacity,
                                                               size_t* out_size);

/* Mount a delegated raw root on a VFS directory. */
struct filesystem_call_result vfs_directory_mount(cap_id_t directory_cap, cap_id_t fs_root_cap, uint32_t flags);

/* Unmount the raw filesystem covering a VFS directory. */
struct filesystem_call_result vfs_directory_unmount(cap_id_t directory_cap);

/* Validate the exact byte sequence used by VFS namespace requests. */
bool vfs_absolute_path_valid(const void* path, size_t path_size);
