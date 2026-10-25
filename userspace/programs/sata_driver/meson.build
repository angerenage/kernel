sata_driver_sources = files(
	'src/commands.c',
	'src/identify.c',
	'src/main.c',
	'src/sata.c',
	'src/service.c',
	'src/transport.c',
)

sata_driver_elf = executable('sata_driver.elf',
	sources : sata_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [device_manager_dep, ahci_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += sata_driver_elf
