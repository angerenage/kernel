freetype_python = find_program('python3', required : true)

freetype_modules = custom_target(
	'userspace_freetype_modules',
	input : 'freetype/modules.cfg',
	output : 'ftmodule.h',
	command : [
		freetype_python,
		files('freetype/builds/meson/parse_modules_cfg.py'),
		'--format=ftmodule.h',
		'@INPUT@',
		'--output',
		'@OUTPUT@',
	],
)

freetype_options = custom_target(
	'userspace_freetype_options',
	input : 'freetype/include/freetype/config/ftoption.h',
	output : 'ftoption.h',
	command : [
		freetype_python,
		files('freetype/builds/meson/process_ftoption_h.py'),
		'@INPUT@',
		'--output=@OUTPUT@',
		'--disable=FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES',
	],
)

freetype_inc = include_directories(
	'.',
	'freetype/include',
	'../../include/libc',
)

freetype_sources = [
	freetype_modules,
	freetype_options,
	files(
		'freetype/src/autofit/autofit.c',
		'freetype/src/base/ftbase.c',
		'freetype/src/base/ftbbox.c',
		'freetype/src/base/ftbdf.c',
		'freetype/src/base/ftbitmap.c',
		'freetype/src/base/ftcid.c',
		'freetype/src/base/ftdebug.c',
		'freetype/src/base/ftfstype.c',
		'freetype/src/base/ftgasp.c',
		'freetype/src/base/ftglyph.c',
		'freetype/src/base/ftgxval.c',
		'freetype/src/base/ftinit.c',
		'freetype/src/base/ftmm.c',
		'freetype/src/base/ftotval.c',
		'freetype/src/base/ftpatent.c',
		'freetype/src/base/ftpfr.c',
		'freetype/src/base/ftstroke.c',
		'freetype/src/base/ftsynth.c',
		'freetype/src/base/ftsystem.c',
		'freetype/src/base/fttype1.c',
		'freetype/src/base/ftwinfnt.c',
		'freetype/src/bdf/bdf.c',
		'freetype/src/cache/ftcache.c',
		'freetype/src/cff/cff.c',
		'freetype/src/cid/type1cid.c',
		'freetype/src/dlg/dlgwrap.c',
		'freetype/src/gzip/ftgzip.c',
		'freetype/src/hvf/hvf.c',
		'freetype/src/lzw/ftlzw.c',
		'freetype/src/pcf/pcf.c',
		'freetype/src/pfr/pfr.c',
		'freetype/src/psaux/psaux.c',
		'freetype/src/pshinter/pshinter.c',
		'freetype/src/psnames/psnames.c',
		'freetype/src/raster/raster.c',
		'freetype/src/sdf/sdf.c',
		'freetype/src/sfnt/sfnt.c',
		'freetype/src/smooth/smooth.c',
		'freetype/src/svg/svg.c',
		'freetype/src/truetype/truetype.c',
		'freetype/src/type1/type1.c',
		'freetype/src/type42/type42.c',
		'freetype/src/winfonts/winfnt.c',
	),
]

freetype_build_args = [
	'-DFT2_BUILD_LIBRARY=1',
	'-DFT_CONFIG_MODULES_H=<ftmodule.h>',
	'-DFT_CONFIG_OPTIONS_H=<ftoption.h>',
]

userspace_freetype = static_library(
	'userspace_freetype',
	sources : freetype_sources,
	c_args : freestanding_c_args + ['-fno-pic', '-fno-pie'] + freetype_build_args + platform_userspace_c_args,
	include_directories : freetype_inc,
	pic : false,
	install : false,
)

userspace_freetype_dep = declare_dependency(
	link_with : userspace_freetype,
	include_directories : include_directories('freetype/include'),
)
