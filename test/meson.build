test_inc = include_directories('../include')
criterion = dependency('criterion', native: true, required: true)
threads = dependency('threads', native: true)

test_common_c_args = ['--coverage', '-DCORE_LOCK_DEBUG=1', '-DBASE_STDLIB_RENAME_ALLOC=1']
test_common_link_args = ['--coverage']

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
		'name': 'message',
		'sources': files(
			'core/message_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'channel',
		'sources': files(
			'core/channel_test.c',
		),
		'suites': ['unit', 'core'],
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
			'heap/heap_test.c',
			'core/id_table_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'sched',
		'sources': files(
			'core/sched_test.c',
		),
		'suites': ['unit', 'core'],
	},

	{
		'name': 'kthread',
		'sources': files(
			'core/kthread_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'thread',
		'sources': files(
			'core/thread_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'uthread',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'core/uthread_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'process',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'core/process_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'cpu_spinlock',
		'sources': files(
			'core/cpu_spinlock_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'cpu_irq',
		'sources': files(
			'core/cpu_irq_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'syscall',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'core/syscall_test.c',
		),
		'suites': ['unit', 'core'],
	},
	{
		'name': 'pmm',
		'sources': files(
			'pmm/pmm_test.c',
			'pmm/basic.c',
			'pmm/contiguous.c',
			'pmm/concurrency.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'vaddr_alloc',
		'sources': files(
			'vaddr_alloc/vaddr_alloc_test.c',
			'vaddr_alloc/basic.c',
			'vaddr_alloc/alignment.c',
			'vaddr_alloc/concurrency.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'heap',
		'sources': files(
			'heap/heap_test.c',
			'heap/basic.c',
			'heap/calloc_realloc.c',
			'heap/concurrency.c',
			'heap/growth.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'vmm',
		'sources': files(
			'vmm/test_support.c',
			'vmm/mock_paging.c',
			'vmm/basic.c',
		),
		'link_with': core_test_lib,
		'suites': ['unit', 'memory'],
	},
]

foreach t : test_suites
	exe = executable(
		t['name'] + '_test',
		sources: t['sources'],
		native: true,
		include_directories: test_inc,
		dependencies: [criterion, threads],
		link_with: t.get('link_with', core_test_lib),
		c_args: test_common_c_args,
		link_args: test_common_link_args,
		install: false,
	)

	test(
		t['name'],
		exe,
		suite: t.get('suites', ['unit']),
		timeout: t.get('timeout', 30),
		is_parallel: t.get('parallel', true),
	)
endforeach
