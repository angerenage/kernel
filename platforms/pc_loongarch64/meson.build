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
	files('../paging_transaction.c'),
	files('../iommu_page_table.c'),
	clang_loongarch64_elf.process(files('entry.asm')),
	clang_loongarch64_elf.process(files('interrupts.asm')),
	clang_loongarch64_elf.process(files('hal/userspace.asm')),
	clang_loongarch64_elf.process(files('thread.asm')),
	files('hal/cache.c'),
	files('hal/cpu.c'),
	files('hal/clock.c'),
	files('hal/hcf.c'),
	files('hal/interrupts/interrupts.c'),
	files('hal/interrupts/controllers.c'),
	files('hal/interrupts/eiointc.c'),
	files('hal/interrupts/fixed.c'),
	files('hal/interrupts/htvec.c'),
	files('hal/interrupts/liointc.c'),
	files('hal/interrupts/pch_lpc.c'),
	files('hal/interrupts/pch_msi.c'),
	files('hal/interrupts/pch_pic.c'),
	files('hal/iommu/iommu.c'),
	files('hal/iommu/v1.c'),
	files('hal/paging.c'),
	files('hal/serial.c'),
	files('hal/syscall.c'),
	files('hal/userspace.c'),
]
