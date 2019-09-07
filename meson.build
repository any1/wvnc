project('wvnc', 'c')

cc = meson.get_compiler('c')
libm = cc.find_library('m', required : false)

wayland_scanner = find_program('wayland-scanner')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
pixman = dependency('pixman-1')
neatvnc = dependency('neatvnc')
libuv = dependency('libuv')
xkbcommon = dependency('xkbcommon')

wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_client = generator(
	wayland_scanner,
	output: '@BASENAME@-client-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

client_protocols = [
	[wl_protocol_dir, 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
	['protocols/wlr-screencopy-unstable-v1.xml'],
	['protocols/wlr-virtual-keyboard-unstable-v1.xml'],
]

client_protos_src = []
client_protos_headers = []

foreach p : client_protocols
	xml = join_paths(p)
	client_protos_src += wayland_scanner_code.process(xml)
	client_protos_headers += wayland_scanner_client.process(xml)
endforeach

lib_client_protos = static_library(
	'client_protos',
	client_protos_src + client_protos_headers,
	dependencies: [wayland_client]
)

client_protos = declare_dependency(
	link_with: lib_client_protos,
	sources: client_protos_headers,
)

executable('wvnc',
	[ 'main.c', 'buffer.c', 'utils.c', 'uinput.c', 'damage.c', ],
	dependencies : [ libm, wayland_client, client_protos, pixman,
		neatvnc, libuv, xkbcommon ],
	install: true)
