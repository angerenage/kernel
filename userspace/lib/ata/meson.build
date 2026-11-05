ata_inc = include_directories('include')

ata_lib = static_library('ata',
	sources : files(
		'src/block_service.c',
		'src/transport.c',
	),
	c_args : userspace_c_args,
	include_directories : [userspace_inc, ata_inc],
	dependencies : [device_manager_dep, ahci_dep],
	install : false,
)

ata_dep = declare_dependency(
	include_directories : ata_inc,
	link_with : ata_lib,
)
