usb_storage_driver_sources = files(
	'src/bot.c',
	'src/main.c',
	'src/service.c',
)

usb_storage_driver_elf = executable('usb_storage_driver.elf',
	sources : usb_storage_driver_sources,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	dependencies : [device_manager_dep, xhci_dep],
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += usb_storage_driver_elf
