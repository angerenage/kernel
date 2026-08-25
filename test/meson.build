test_inc = include_directories('../include')
loader_test_inc = include_directories('../userspace/include')
criterion = dependency('criterion', native: true, required: true)
threads = dependency('threads', native: true)

test_common_c_args = ['--coverage', '-DCORE_LOCK_DEBUG=1', '-DBASE_STDLIB_RENAME_ALLOC=1']
test_common_link_args = ['--coverage']
elf_loader_test_c_args = []
if platform == 'pc_x86_64'
	elf_loader_test_c_args += ['-DPLATFORM_PC_X86_64']
elif platform == 'pc_aarch64'
	elf_loader_test_c_args += ['-DPLATFORM_PC_AARCH64']
elif platform == 'pc_riscv64'
	elf_loader_test_c_args += ['-DPLATFORM_PC_RISCV64']
elif platform == 'pc_loongarch64'
	elf_loader_test_c_args += ['-DPLATFORM_PC_LOONGARCH64']
endif


if test_sanitizers
	test_common_c_args += [
		'-fsanitize=address,undefined',
		'-fno-omit-frame-pointer',
		'-fno-sanitize-recover=all',
	]
	test_common_link_args += ['-fsanitize=address,undefined']
endif

test_environment = environment()
if test_sanitizers
	# Kernel allocations come from the test-owned heap arena, which LeakSanitizer cannot track. Some lifecycle tests
	# intentionally leave scheduler-owned objects live until process exit, so only enable the address and UB checks.
	test_environment.set('ASAN_OPTIONS', 'detect_leaks=0')
endif

base_test_lib = static_library(
	'base_test_lib',
	base_sources,
	include_directories: test_inc,
	native: true,
	c_args: test_common_c_args,
	link_args: test_common_link_args,
	install: false,
)

hal_mock_sources = files(
	'mocks/base/heap.c',
	'mocks/base/heap_hooks.c',
	'mocks/base/display.c',
	'mocks/hal/clock.c',
	'mocks/hal/cpu.c',
	'mocks/hal/hcf.c',
	'mocks/hal/interrupts.c',
	'mocks/hal/paging.c',
	'mocks/hal/userspace.c',
	'mocks/kernel/boot.c',
)

core_test_lib = static_library(
	'core_test_lib',
	base_sources + core_sources + hal_mock_sources,
	include_directories: test_inc,
	native: true,
	c_args: test_common_c_args,
	link_args: test_common_link_args,
	install: false,
)

test_suites = [
	{
		'name': 'base_string',
		'sources': files(
			'base/string_test.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'base'],
	},
	{
		'name': 'base_math',
		'sources': files(
			'base/math_test.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'base'],
	},
	{
		'name': 'base_time',
		'sources': files(
			'base/time_test.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'base'],
	},
	{
		'name': 'condvar',
		'sources': files(
			'core/condvar_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'mutex',
		'sources': files(
			'core/mutex_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'semaphore',
		'sources': files(
			'core/semaphore_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'ipc',
		'sources': files(
			'ipc/test_support.c',
			'ipc/signal.c',
			'ipc/interrupt.c',
			'ipc/ring_buffer.c',
			'ipc/channel_lifecycle.c',
			'ipc/capability_queue.c',
			'ipc/channel_state.c',
			'../kernel/src/capability/signal.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'capability',
		'sources': files(
			'capability/test_support.c',
			'capability/objects.c',
			'capability/capabilities.c',
			'capability/calls.c',
			'capability/authorization.c',
			'capability/delegation.c',
			'capability/revocation.c',
			'capability/lifecycle.c',
			'capability/target_lifetime.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'loader_lifecycle',
		'sources': files(
			'loader/lifecycle.c',
			'../userspace/programs/loader/lifecycle.c',
		),
		'include_directories': [loader_test_inc],
		'link_with': base_test_lib,
		'suites': ['unit', 'userspace'],
	},
	{
		'name': 'rwlock',
		'sources': files(
			'core/rwlock_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'id_table',
		'sources': files(
			'heap/test_support.c',
			'core/id_table_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'sched',
		'sources': files(
			'sched/test_support.c',
			'sched/initialization.c',
			'sched/dispatch.c',
			'sched/preemption.c',
			'sched/placement.c',
			'sched/statistics.c',
			'sched/wait_queue.c',
			'sched/cancellation.c',
			'sched/sleep_interrupt.c',
			'sched/run_queue.c',
			'sched/lifecycle.c',
		),
		'suites': ['unit', 'core'],
	},

	{
		'name': 'kthread',
		'sources': files(
			'kthread/test_support.c',
			'kthread/initialization.c',
			'kthread/scheduling.c',
			'kthread/lifecycle.c',
			'kthread/park.c',
			'kthread/join.c',
			'kthread/cancellation.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'thread',
		'sources': files(
			'thread/test_support.c',
			'thread/initialization.c',
			'thread/lifecycle.c',
			'thread/cancellation.c',
			'thread/interrupt.c',
			'thread/reap.c',
			'thread/run_queue.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'uthread',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'uthread/test_support.c',
			'uthread/start.c',
			'uthread/current.c',
			'uthread/lifecycle.c',
			'uthread/arguments.c',
			'uthread/rollback.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'process',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'process/test_support.c',
			'process/creation.c',
			'process/threads.c',
			'process/lifecycle.c',
			'process/concurrency.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'cpu',
		'sources': files(
			'cpu/test_support.c',
			'cpu/topology.c',
			'cpu/state.c',
			'cpu/interrupts.c',
			'cpu/spinlock.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'syscall',
		'sources': files(
			'mocks/core/syscall_dispatch.c',
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'syscall/test_support.c',
			'syscall/basic.c',
			'syscall/capability.c',
			'syscall/capability_rollback.c',
			'syscall/time.c',
			'syscall/thread.c',
			'syscall/channel.c',
			'syscall/validation.c',
			'../kernel/src/syscall/capability.c',
			'../kernel/src/syscall/channel.c',
			'../kernel/src/syscall/misc.c',
			'../kernel/src/syscall/module.c',
			'../kernel/src/syscall/process.c',
			'../kernel/src/syscall/thread.c',
			'../kernel/src/syscall/time.c',
			'../kernel/src/syscall/upcall.c',
			'../kernel/src/capability/signal.c',
		),
		'suites': ['unit', 'kernel'],
	},
	{
		'name': 'syscall_dispatch',
		'sources': files(
			'syscall/dispatch.c',
			'../kernel/src/syscall/syscall.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'kernel'],
	},
	{
		'name': 'elf_loader',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'elf_loader/test_support.c',
			'elf_loader/format.c',
			'elf_loader/segments.c',
			'elf_loader/rollback.c',
			'../kernel/src/elf_loader.c',
		),
		'c_args': elf_loader_test_c_args,
		'suites': ['unit', 'kernel'],
	},
	{
		'name': 'kernel_capability',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'syscall/test_support.c',
			'kernel_capability/test_support.c',
			'kernel_capability/memory.c',
			'kernel_capability/serial.c',
			'kernel_capability/module.c',
			'../kernel/src/capability/capability.c',
			'../kernel/src/capability/memory.c',
			'../kernel/src/capability/boot_module.c',
			'../kernel/src/capability/serial.c',
			'../kernel/src/syscall/capability.c',
			'../kernel/src/syscall/memory.c',
			'../kernel/src/syscall/module.c',
		),
		'suites': ['unit', 'kernel'],
	},
	{
		'name': 'pmm',
		'sources': files(
			'pmm/test_support.c',
			'pmm/allocation.c',
			'pmm/contiguous.c',
			'pmm/memory_map.c',
			'pmm/free.c',
			'pmm/concurrency.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'heap',
		'sources': files(
			'heap/test_support.c',
			'heap/allocation.c',
			'heap/strings.c',
			'heap/calloc_realloc.c',
			'heap/concurrency.c',
			'heap/coalescing.c',
			'heap/failure.c',
			'heap/growth.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'upcall_syscall',
		'sources': files(
			'core/upcall_syscall_test.c',
			'../kernel/src/syscall/upcall.c',
			'../kernel/src/syscall/user_frame.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'kernel'],
	},
	{
		'name': 'user_upcall',
		'sources': files(
			'core/user_upcall_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'exception_policy',
		'sources': files(
			'exception/policy.c',
			'../core/exception.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'core'],
	},
	{
		'name': 'exception_fault',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'exception/page_fault.c',
		),
		'link_with': core_test_lib,
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'user_return',
		'sources': files(
			'core/user_return_test.c',
			'../core/user_return.c',
		),
		'link_with': base_test_lib,
		'suites': ['unit', 'core'],
	},
	{
		'name': 'address_transfer',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'address_transfer/logical.c',
		),
		'link_with': core_test_lib,
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'vmm',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'vmm/architecture.c',
		),
		'link_with': core_test_lib,
		'suites': ['unit', 'memory'],
	},
]

if build_machine.cpu_family() == 'aarch64'
	test_suites += [
		{
			'name': 'aarch64_exception_return',
			'sources': files(
				'platform/aarch64/exception_return_test.c',
			),
			'link_with': base_test_lib,
			'suites': ['unit', 'platform'],
		},
	]
endif

if build_machine.cpu_family() == 'x86_64'
	test_suites += [
		{
			'name': 'x86_64_boot',
			'sources': files(
				'boot/test_support.c',
				'boot/state.c',
				'boot/topology.c',
			),
			'link_with': base_test_lib,
			'suites': ['unit', 'boot', 'platform'],
		},
		{
			'name': 'x86_64_syscall',
			'sources': files(
				'platform/x86_64/syscall_test.c',
				'../platforms/pc_x86_64/hal/syscall.c',
			),
			'link_with': base_test_lib,
			'suites': ['unit', 'platform'],
		},
		{
			'name': 'x86_64_userspace',
			'sources': files(
				'platform/x86_64/userspace_test.c',
				'../platforms/pc_x86_64/hal/userspace.c',
			),
			'link_with': base_test_lib,
			'suites': ['unit', 'platform'],
		},
	]
endif

foreach t : test_suites
	exe = executable(
		t['name'] + '_test',
		sources: t['sources'],
		native: true,
		include_directories: [test_inc] + t.get('include_directories', []),
		dependencies: [criterion, threads],
		link_with: t.get('link_with', core_test_lib),
		c_args: test_common_c_args + t.get('c_args', []),
		link_args: test_common_link_args,
		install: false,
	)

	test(
		t['name'],
		exe,
		suite: t.get('suites', ['unit']),
		timeout: t.get('timeout', 120),
		is_parallel: t.get('parallel', true),
		env: test_environment,
	)
endforeach
