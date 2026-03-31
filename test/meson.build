test_inc = include_directories('../include')
criterion = dependency('criterion', native: true, required: true)
threads = dependency('threads', native: true)

test_common_c_args = ['--coverage']
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

core_test_lib = static_library(
	'core_test_lib',
	base_sources + core_kernel_sources,
	include_directories: test_inc,
	native: true,
	c_args: test_common_c_args,
	link_args: test_common_link_args,
	install: false,
)

kernel_test_lib = static_library(
	'kernel_test_lib',
	base_sources + core_sources + files('../kernel/src/memory/vmm.c'),
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
		'name': 'kheap',
		'sources': files(
			'kheap/kheap_test.c',
			'kheap/basic.c',
			'kheap/calloc_realloc.c',
			'kheap/concurrency.c',
			'kheap/growth.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'vmm',
		'sources': files(
			'vmm/vmm_test.c',
			'vmm/mock_paging.c',
			'vmm/basic.c',
			'vmm/rollback.c',
		),
		'link_with': kernel_test_lib,
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
