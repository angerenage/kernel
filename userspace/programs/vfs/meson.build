vfs_include = include_directories('include')

vfs_client = static_library('vfs_client',
	sources : files('lib/vfs.c'),
	c_args : userspace_c_args,
	include_directories : [userspace_runtime_inc, vfs_include],
	pic : false,
	install : false,
)

vfs_dep = declare_dependency(
	include_directories : vfs_include,
	link_with : vfs_client,
)

vfs_src = files('src/main.c', 'src/server.c')

vfs_elf = executable('vfs.elf',
	sources : vfs_src,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : vfs_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += vfs_elf
