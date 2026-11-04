hid_driver_sources = files(
    'src/main.c',
    'src/input.c'
)

hid_driver_elf = executable('hid_driver.elf',
    sources : hid_driver_sources,
    c_args : userspace_c_args,
    include_directories : userspace_inc,
    dependencies : [device_manager_dep, xhci_dep],
    link_whole : userspace_crt,
    link_with : userspace_link_libs,
    link_args : userspace_link_args,
    install : false,
    link_depends : files(userspace_linker_file),
)

userspace_programs += hid_driver_elf
