pci_driver_sources = files(
	'src/claim.c',
	'src/config.c',
	'src/device.c',
	'src/ecam.c',
	'src/enumerate.c',
	'src/host.c',
	'src/main.c',
	'src/pci.c',
	'src/resource.c',
)

pci_driver_elf = executable('pci_driver.elf',
	sources : pci_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += pci_driver_elf
