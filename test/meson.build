test_inc = include_directories('../include')
criterion = dependency('criterion', native: true, required: true)

test_common_c_args = ['--coverage']
test_common_link_args = ['--coverage']

core_test_lib = static_library(
	'core_test_lib',
	core_kernel_sources,
	include_directories: test_inc,
	native: true,
	c_args: test_common_c_args,
	link_args: test_common_link_args,
	install: false,
)

test_suites = [
	{
		'name': 'early_alloc',
		'sources': files(
			'early_alloc/early_alloc_test.c',
			'early_alloc/basic_allocations.c',
			'early_alloc/allocation_until_exhaustion.c',
		),
		'suites': ['unit', 'memory'],
	},
	{
		'name': 'pmm',
		'sources': files(
			'pmm/pmm_test.c',
			'pmm/basic.c',
			'pmm/contiguous.c',
		),
		'suites': ['unit', 'memory'],
	},
]

foreach t : test_suites
	exe = executable(
		t['name'] + '_test',
		sources: t['sources'],
		native: true,
		include_directories: test_inc,
		dependencies: [criterion],
		link_with: core_test_lib,
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
