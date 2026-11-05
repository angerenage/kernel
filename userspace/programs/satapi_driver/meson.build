satapi_driver_elf = executable('satapi_driver.elf',
	sources : files(
		'src/commands.c',
		'src/main.c',
		'src/satapi.c',
	),
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [device_manager_dep, ahci_dep, ata_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += satapi_driver_elf
