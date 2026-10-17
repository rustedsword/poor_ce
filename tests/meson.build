test_runner = static_library(
	'test-runner',
	sources: [
		'test-runner.c',
		'test-helpers.c',
	],
	include_directories: root_inc,
	dependencies: [
		cc.find_library('dl', required: false),
		dependency('threads'),
		poor_base_dep,
	]
)

test_runner_dep = declare_dependency(
	link_with: test_runner,
	include_directories: ce_inc,
	dependencies: [
		dependency('threads'),
		cc.find_library('dl', required: false),
		poor_base_dep,
	]
)

test(
	'text-test',
	executable('text-test', 'text-test.c', text_sources, font_sources,
		   dependencies: [ test_runner_dep, freetype_dep, cc.find_library('m', required: false) ]),
	env: [ 'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()) ],
)

input_test = executable(
	'input-test',
	'input-test.c',
	'input-server.c',
	log_sources,
	seat_sources,
	client_protocol_h,
	server_protocol_h,
	protocol_c,
	dependencies: [ test_runner_dep, client_dep, server_dep, poor_loop_dep, xkbcommon_dep ],
)

# Each test with input runs with each ring setup
foreach ring : [ 'default', 'coop', 'defer', 'sqpoll' ]
	test(
		'input-test-' + ring,
		input_test,
		env: [ 'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()), 'TEST_RING=' + ring ],
	)
endforeach

# Vulkan tests need the validation layer, which fails them on any error. Mesa's device-select layer would connect
# to the compositor of the session.
validation_runner = find_program('python3', version: '>= 3.10', native: true)
validation_check = files('check-validation.py')
vulkan_env = [ 'NODEVICE_SELECT=1', 'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()) ]
test('validation-checker', validation_runner, args: files('check_validation_test.py'))
vulkan_validation_probe = executable('vulkan-validation-probe', 'vulkan-validation-probe.c',
	dependencies: [ vulkan_dep, poor_base_dep ])
test(
	'validation-reports-errors',
	validation_runner,
	args: [ validation_check, '--vulkan', vulkan_validation_probe, vulkan_validation_probe, '--report-error' ],
	env: vulkan_env,
	should_fail: true,
)
render_test = executable(
	'render-test',
	'render-test.c',
	text_sources,
	font_sources,
	log_sources,
	render_sources,
	lsp_sources,
	editor_sources,
	shaders_h,
	client_protocol_h,
	dependencies: [ test_runner_dep, swapchain_dep, client_dep, poor_loop_dep, vulkan_dep, vulkan_initializers_dep,
			freetype_dep, xkbcommon_dep, cc.find_library('m', required: false) ],
)
fake_lsp = executable('fake-lsp', 'fake-lsp.c', dependencies: [ poor_base_dep ])
lsp_test = executable('lsp-test', 'lsp-test.c', text_sources, log_sources, lsp_sources,
		      dependencies: [ test_runner_dep, poor_loop_dep, cc.find_library('m', required: false) ])
editor_test = executable('editor-test', 'editor-test.c', text_sources, font_sources, log_sources, lsp_sources,
			 editor_sources, client_protocol_h,
			 dependencies: [ test_runner_dep, poor_loop_dep, client_dep, xkbcommon_dep, freetype_dep,
					 cc.find_library('m', required: false) ])
lsp_env = [ 'TEST_BUILD_DIR=@0@'.format(meson.current_build_dir()), 'FAKE_LSP=@0@'.format(fake_lsp.full_path()) ]

# The app runs with the defer setup, its parts are tested with it and without flags
foreach ring : [ 'default', 'defer' ]
	test(
		'render-test-' + ring,
		validation_runner,
		args: [ validation_check, '--vulkan', vulkan_validation_probe, render_test ],
		env: vulkan_env + [ 'TEST_RING=' + ring ],
	)
	test('lsp-test-' + ring, lsp_test, env: lsp_env + [ 'TEST_RING=' + ring ], depends: fake_lsp)
	test('editor-test-' + ring, editor_test, env: lsp_env + [ 'TEST_RING=' + ring ], depends: fake_lsp)
endforeach

# Input for trying the editor on a desktop, see drive.sh
executable('fake-input', 'fake-input.c', dependencies: [ poor_base_dep ])
