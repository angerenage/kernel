acpi_parser_src = files(
	'src/main.c',
	'src/parser.c',
	'src/device.c',
	'src/tables/mcfg.c',
	'src/tables/tpm2.c',
)

acpi_parser_elf = executable('acpi_parser.elf',
	sources : acpi_parser_src,
	c_args : userspace_c_args,
	include_directories : userspace_inc,
	link_whole : userspace_crt,
	link_with : userspace_link_libs,
	link_args : userspace_link_args,
	install : false,
	link_depends : files(userspace_linker_file),
)

userspace_programs += acpi_parser_elf
