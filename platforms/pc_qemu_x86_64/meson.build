platform_kernel_c_args += [
	'-DPLATFORM_PC_QEMU_X86_64',
]

platform_kernel_objects += [
	nasm_elf64.process(files('entry.asm')),
	files('serial.c'),
	files('memory/mm.c'),
	files('memory/early_alloc.c'),
]
