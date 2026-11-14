block_lib = static_library('block_client',
	sources : files('src/block.c'),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : device_manager_dep,
	pic : false,
	install : false,
)

block_dep = declare_dependency(
	dependencies : device_manager_dep,
	link_with : block_lib,
)
