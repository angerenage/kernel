xhci_driver_sources = files(
	'src/main.c',
	'src/xhci.c',
	'src/controller.c',
	'src/dma.c',
	'src/ring.c',
)

xhci_driver_elf = executable('xhci_driver.elf',
	sources : xhci_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : device_manager_dep,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += xhci_driver_elf
