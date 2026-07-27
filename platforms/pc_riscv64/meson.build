platform_limine_arch = 'riscv64'

platform_kernel_c_args += [
	'-DPLATFORM_PC_RISCV64',
	'-march=rv64imac',
	'-mabi=lp64',
]

platform_userspace_c_args += [
	'-march=rv64gcv',
	'-mabi=lp64d',
	'-mrvv-vector-bits=128',
]

platform_kernel_objects += [
	clang_riscv64_elf.process(files('entry.asm')),
	clang_riscv64_elf.process(files('interrupts.asm')),
	clang_riscv64_elf.process(files('hal/userspace.asm')),
	clang_riscv64_elf.process(files('thread.asm')),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
	files('hal/syscall.c'),
	files('hal/userspace.c'),
]
