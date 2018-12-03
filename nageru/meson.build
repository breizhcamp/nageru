qt5 = import('qt5')
protoc = find_program('protoc')
cxx = meson.get_compiler('cpp')

embedded_bmusb = get_option('embedded_bmusb')

alsadep = dependency('alsa')
bmusbdep = dependency('bmusb', required: not embedded_bmusb)
dldep = cxx.find_library('dl')
epoxydep = dependency('epoxy')
libavcodecdep = dependency('libavcodec')
libavformatdep = dependency('libavformat')
libavresampledep = dependency('libavresample')
libavutildep = dependency('libavutil')
libjpegdep = dependency('libjpeg')
libswscaledep = dependency('libswscale')
libusbdep = dependency('libusb-1.0')
luajitdep = dependency('luajit')
movitdep = dependency('movit')
protobufdep = dependency('protobuf')
qcustomplotdep = cxx.find_library('qcustomplot')
qt5deps = dependency('qt5', modules: ['Core', 'Gui', 'Widgets', 'OpenGLExtensions', 'OpenGL', 'PrintSupport'])
threaddep = dependency('threads')
vadrmdep = dependency('libva-drm')
vax11dep = dependency('libva-x11')
x11dep = dependency('x11')
x264dep = dependency('x264')
zitaresamplerdep = cxx.find_library('zita-resampler')

srcs = []
nageru_deps = [shareddep, qt5deps, libjpegdep, movitdep, protobufdep,
	vax11dep, vadrmdep, x11dep, libavformatdep, libavresampledep, libavcodecdep, libavutildep,
	libswscaledep, libusbdep, luajitdep, dldep, x264dep, alsadep, zitaresamplerdep,
	qcustomplotdep, threaddep]
nageru_include_dirs = []
nageru_link_with = []
nageru_build_rpath = ''
nageru_install_rpath = ''

kaeru_link_with = []
kaeru_extra_deps = []

# CEF.
exe_dir = join_paths(get_option('prefix'), 'lib/nageru')
cef_dir = get_option('cef_dir')
cef_build_type = get_option('cef_build_type')
have_cef = (cef_dir != '')
if have_cef
	add_project_arguments('-DHAVE_CEF=1', language: 'cpp')

	system_cef = (cef_build_type == 'system')
	if system_cef
		cef_lib_dir = cef_dir
		cef_resource_dir = '/usr/share/cef/Resources'
	else
		cef_lib_dir = join_paths(cef_dir, cef_build_type)
		cef_resource_dir = join_paths(cef_dir, 'Resources')

		nageru_include_dirs += include_directories(cef_dir)
		nageru_include_dirs += include_directories(join_paths(cef_dir, 'include'))
		nageru_build_rpath = cef_lib_dir
		nageru_install_rpath = '$ORIGIN/'
	endif

	cefdep = cxx.find_library('cef')
	nageru_deps += cefdep

	# CEF wrapper library; not built as part of the CEF binary distribution,
	# but should be if CEF is installed as a system library.
	if system_cef
		cefdlldep = cxx.find_library('cef_dll_wrapper')
		nageru_deps += cefdlldep
	else
		cmake = find_program('cmake')
		cef_compile_script = find_program('scripts/compile_cef_dll_wrapper.sh')

		cef_dll_target = custom_target('libcef_dll_wrapper',
			input: join_paths(cef_dir, 'libcef_dll/CMakeLists.txt'),
			output: ['libcef_dll_wrapper.a', 'cef-stamp'],
			command: [cef_compile_script, '@BUILD_DIR@', cef_dir, cmake, '@OUTPUT@'])

		# Putting the .a in sources seemingly hits a bug where the .a files get sorted
		# in the wrong order. This is a workaround; see
		# https://github.com/mesonbuild/meson/issues/3613#issuecomment-408276296 .
		cefdlldep = declare_dependency(sources: cef_dll_target[1], link_args: cef_dll_target.full_path())
		nageru_deps += cefdlldep
	endif

	cef_libs = ['libEGL.so', 'libGLESv2.so', 'natives_blob.bin', 'snapshot_blob.bin', 'v8_context_snapshot.bin']
	cef_resources = ['cef.pak', 'cef_100_percent.pak', 'cef_200_percent.pak', 'cef_extensions.pak', 'devtools_resources.pak']
	if not get_option('cef_no_icudtl')
		cef_resources += ['icudtl.dat']
	endif
	if cef_build_type != 'system'
		cef_libs += ['libcef.so']
	endif

	# Symlink the files into the build directory, so that running nageru without ninja install works.
	run_command('mkdir', join_paths(meson.current_build_dir(), 'locales/'))
	foreach file : cef_libs
		run_command('ln', '-s', join_paths(cef_lib_dir, file), meson.current_build_dir())
		install_data(join_paths(cef_lib_dir, file), install_dir: exe_dir)
	endforeach
	foreach file : cef_resources
		run_command('ln', '-s', join_paths(cef_resource_dir, file), meson.current_build_dir())
		install_data(join_paths(cef_resource_dir, file), install_dir: exe_dir)
	endforeach
	run_command('ln', '-s', join_paths(cef_resource_dir, 'locales/en-US.pak'), join_paths(meson.current_build_dir(), 'locales/'))
	install_data(join_paths(cef_resource_dir, 'locales/en-US.pak'), install_dir: join_paths(exe_dir, 'locales'))
endif

# bmusb.
if embedded_bmusb
	bmusb_dir = include_directories('bmusb')
	nageru_include_dirs += bmusb_dir

	bmusb = static_library('bmusb', 'bmusb/bmusb.cpp', 'bmusb/fake_capture.cpp',
		dependencies: [libusbdep],
		include_directories: [bmusb_dir])
	nageru_link_with += bmusb
	kaeru_link_with += bmusb
else
	nageru_deps += bmusbdep
	kaeru_extra_deps += bmusbdep
endif

# Protobuf compilation.
gen = generator(protoc, \
	output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
	arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process(['state.proto', 'midi_mapping.proto', 'json.proto'])
protobuf_lib = static_library('protobufs', proto_generated, dependencies: nageru_deps, include_directories: nageru_include_dirs)
protobuf_hdrs = declare_dependency(sources: proto_generated)
nageru_link_with += protobuf_lib

# Preprocess Qt as needed.
qt_files = qt5.preprocess(
	moc_headers: ['aboutdialog.h', 'analyzer.h', 'clickable_label.h', 'compression_reduction_meter.h', 'correlation_meter.h',
		'ellipsis_label.h', 'glwidget.h', 'input_mapping_dialog.h', 'lrameter.h', 'mainwindow.h', 'midi_mapping_dialog.h',
		'nonlinear_fader.h', 'vumeter.h'],
	ui_files: ['aboutdialog.ui', 'analyzer.ui', 'audio_expanded_view.ui', 'audio_miniview.ui', 'display.ui',
		'input_mapping.ui', 'mainwindow.ui', 'midi_mapping.ui'],
	dependencies: qt5deps)

# Qt objects.
srcs += ['glwidget.cpp', 'mainwindow.cpp', 'vumeter.cpp', 'lrameter.cpp', 'compression_reduction_meter.cpp',
	'correlation_meter.cpp', 'aboutdialog.cpp', 'analyzer.cpp', 'input_mapping_dialog.cpp', 'midi_mapping_dialog.cpp',
	'nonlinear_fader.cpp', 'context_menus.cpp', 'vu_common.cpp', 'piecewise_interpolator.cpp', 'midi_mapper.cpp']

# Auxiliary objects used for nearly everything.
aux_srcs = ['flags.cpp']
aux = static_library('aux', aux_srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += aux

# Audio objects.
audio_mixer_srcs = ['audio_mixer.cpp', 'alsa_input.cpp', 'alsa_pool.cpp', 'ebu_r128_proc.cc', 'stereocompressor.cpp',
	'resampling_queue.cpp', 'flags.cpp', 'correlation_measurer.cpp', 'filter.cpp', 'input_mapping.cpp']
audio = static_library('audio', audio_mixer_srcs, dependencies: [nageru_deps, protobuf_hdrs], include_directories: nageru_include_dirs)
nageru_link_with += audio

# Mixer objects.
srcs += ['chroma_subsampler.cpp', 'v210_converter.cpp', 'mixer.cpp', 'pbo_frame_allocator.cpp',
	'theme.cpp', 'image_input.cpp', 'alsa_output.cpp',
	'timecode_renderer.cpp', 'tweaked_inputs.cpp']

# Streaming and encoding objects (largely the set that is shared between Nageru and Kaeru).
stream_srcs = ['quicksync_encoder.cpp', 'x264_encoder.cpp', 'x264_dynamic.cpp', 'x264_speed_control.cpp', 'video_encoder.cpp',
	'audio_encoder.cpp', 'ffmpeg_util.cpp', 'ffmpeg_capture.cpp',
	'print_latency.cpp', 'basic_stats.cpp', 'ref_counted_frame.cpp']
stream = static_library('stream', stream_srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += stream

# DeckLink.
srcs += ['decklink_capture.cpp', 'decklink_util.cpp', 'decklink_output.cpp',
	'decklink/DeckLinkAPIDispatch.cpp']
decklink_dir = include_directories('decklink')
nageru_include_dirs += decklink_dir

# CEF input.
if have_cef
	srcs += ['nageru_cef_app.cpp', 'cef_capture.cpp']
endif

srcs += qt_files
srcs += proto_generated

# Everything except main.cpp. (We do this because if you specify a .cpp file in
# both Nageru and Kaeru, it gets compiled twice. In the older Makefiles, Kaeru
# depended on a smaller set of objects.)
core = static_library('core', srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += core

# Nageru executable; it goes into /usr/lib/nageru since CEF files go there, too
# (we can't put them straight into /usr/bin).
executable('nageru', 'main.cpp',
	dependencies: nageru_deps,
	include_directories: nageru_include_dirs,
	link_with: nageru_link_with,
	build_rpath: nageru_build_rpath,
	install_rpath: nageru_install_rpath,
	install: true,
	install_dir: exe_dir
)
meson.add_install_script('scripts/setup_nageru_symlink.sh')

# Kaeru executable.
executable('kaeru', 'kaeru.cpp',
	dependencies: [nageru_deps, kaeru_extra_deps],
	include_directories: nageru_include_dirs,
	link_with: [stream, aux, kaeru_link_with],
	install: true)

# Audio mixer microbenchmark.
executable('benchmark_audio_mixer', 'benchmark_audio_mixer.cpp', dependencies: nageru_deps, include_directories: nageru_include_dirs, link_with: [audio, aux])

# These are needed for a default run.
data_files = ['theme.lua', 'simple.lua', 'bg.jpeg', 'akai_midimix.midimapping']
install_data(data_files, install_dir: join_paths(get_option('prefix'), 'share/nageru'))
foreach file : data_files
	run_command('ln', '-s', join_paths(meson.current_source_dir(), file), meson.current_build_dir())
endforeach
