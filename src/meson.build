protocol_xmls = {
	'xdg-shell': protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
	'xdg-decoration-unstable-v1': protocols_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
	'viewporter': protocols_dir / 'stable/viewporter/viewporter.xml',
	'fractional-scale-v1': protocols_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
	'cursor-shape-v1': protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	'tablet-unstable-v2': protocols_dir / 'unstable/tablet/tablet-unstable-v2.xml',
	'primary-selection-unstable-v1': protocols_dir / 'unstable/primary-selection/primary-selection-unstable-v1.xml',
}

client_protocol_h = []
server_protocol_h = []
protocol_c = []
foreach name, xml : protocol_xmls
	client_protocol_h += custom_target(
		name + ' client header',
		command: [ scanner, '-s', 'client-header', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: name + '-client-protocol.h',
	)
	server_protocol_h += custom_target(
		name + ' server header',
		command: [ scanner, '-s', 'server-header', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: name + '-server-protocol.h',
	)
	protocol_c += custom_target(
		name + ' protocol source',
		command: [ scanner, '-s', 'private-code', '@INPUT@', '@OUTPUT@' ],
		input: xml,
		output: name + '-protocol.c',
	)
endforeach

shaders_h = []
foreach shader : [ 'text.vert', 'text.frag' ]
	shaders_h += custom_target(
		shader,
		command: [ glslang, '-V', '--target-env', 'vulkan1.4', '--vn', shader.underscorify(), '-o', '@OUTPUT@', '@INPUT@' ],
		input: shader,
		output: shader + '.h',
	)
endforeach

viewer_inc = include_directories('.')
text_sources = files('text.c', 'view.c', 'history.c', 'document.c', 'syntax.c', 'json.c', 'path.c')
font_sources = files('font.c', 'atlas.c')
log_sources = files('logger.c')
seat_sources = files('seat.c')
render_sources = files('render.c')
lsp_sources = files('lsp.c')
editor_sources = files('tree.c', 'editor.c', 'scene.c')

executable(
	'poor_text_viewer',
	'main.c',
	text_sources,
	font_sources,
	log_sources,
	seat_sources,
	render_sources,
	lsp_sources,
	editor_sources,
	client_protocol_h,
	protocol_c,
	shaders_h,
	dependencies: [ swapchain_dep, client_dep, poor_loop_dep, poor_base_dep, vulkan_dep, vulkan_initializers_dep,
			xkbcommon_dep, freetype_dep, cc.find_library('m', required: false) ],
	install: true,
)
