ahci_include = include_directories('include')

ahci_dep = declare_dependency(
	include_directories : ahci_include,
)

ahci_driver_sources = files(
	'src/ahci.c',
	'src/command.c',
	'src/dma.c',
	'src/main.c',
	'src/port.c',
	'src/transport.c',
)

ahci_driver_elf = executable('ahci_driver.elf',
	sources : ahci_driver_sources,
	c_args : userspace_c_args,
	include_directories : [userspace_inc, ahci_include],
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += ahci_driver_elf
