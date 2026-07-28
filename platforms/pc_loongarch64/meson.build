platform_limine_arch = 'loongarch64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_LOONGARCH64',
	'--target=loongarch64-unknown-none-gnusf',
	'-mno-lsx',
	'-mno-lasx',
]

platform_kernel_link_args += [
	'--target=loongarch64-unknown-none-gnusf',
]

platform_userspace_c_args += [
	'-mabi=lp64d',
	'-mlasx',
]

platform_kernel_objects += [
	clang_loongarch64_elf.process(files('entry.asm')),
	clang_loongarch64_elf.process(files('interrupts.asm')),
	clang_loongarch64_elf.process(files('hal/userspace.asm')),
	clang_loongarch64_elf.process(files('thread.asm')),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
	files('hal/syscall.c'),
	files('hal/userspace.c'),
]
