project(
	'poor_wayland_text_viewer', 'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>= 1.4.0',
	default_options: [
		'warning_level=2',
		'buildtype=debugoptimized',
		'c_std=c23',
		'default_library=static',
	]
)

if host_machine.system() != 'linux'
	error('poor_wayland runs on io_uring, which needs Linux')
endif

config_h = configuration_data()
config_h.set_quoted('PACKAGE', meson.project_name())
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())

cc = meson.get_compiler('c')
add_project_arguments(
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wstrict-prototypes',
		'-Wmissing-prototypes',
		# Array views of no elements are variable length arrays of zero length
		'-fno-sanitize=vla-bound',
	]),
	language: 'c'
)

foreach h: [ 'sys/prctl.h' ]
	config_h.set('HAVE_' + h.underscorify().to_upper(), cc.has_header(h))
endforeach

foreach f: [ 'prctl' ]
	config_h.set('HAVE_' + f.underscorify().to_upper(), cc.has_function(f))
endforeach

configure_file(
	output: 'config.h',
	configuration: config_h,
)

if not cc.has_header_symbol('drm/drm.h', 'DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE')
	error('the swapchain needs the kernel headers of Linux 6.16 or newer')
endif

# Installed poor_wayland, poor_wayland_swapchain and wayland-protocols are used if there are, the submodules otherwise
swapchain_dep = dependency('poor_wayland_swapchain_vulkan', fallback: 'poor_wayland_swapchain',
	default_options: [ 'tests=false', 'examples=false' ])
client_dep = dependency('poor_wayland_client', fallback: 'poor_wayland', default_options: [ 'tests=false' ])
scanner_dep = dependency('poor_wayland_scanner', native: true, fallback: 'poor_wayland', default_options: [ 'tests=false' ])
scanner = find_program(scanner_dep.get_variable('poor_wayland_scanner'), native: true)
poor_loop_dep = dependency('poor_loop')
# The submodules take an older installed liburing too, but pipes need 2.12
dependency('liburing', version: '>= 2.12',
	not_found_message: 'the submodule is built instead with --force-fallback-for=liburing')
poor_base_dep = dependency('poor_base')
wayland_protocols_dep = dependency('wayland-protocols', version: '>= 1.49', fallback: 'wayland-protocols',
	default_options: [ 'tests=false' ])
protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')
vulkan_dep = dependency('vulkan')
vulkan_initializers_dep = dependency('vulkan_initializers')
xkbcommon_dep = dependency('xkbcommon')
freetype_dep = dependency('freetype2')
glslang = find_program('glslangValidator', native: true)

root_inc = include_directories('.')

subdir('src')

if get_option('tests')
	server_dep = dependency('poor_wayland_server', fallback: 'poor_wayland', default_options: [ 'tests=false' ])
	subdir('tests')
endif
