project('nageru', 'cpp', default_options: ['buildtype=debugoptimized'], version: '1.8.0')

cxx = meson.get_compiler('cpp')
qt5 = import('qt5')
protoc = find_program('protoc')

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
qt5deps = dependency('qt5', modules: ['Core', 'Gui', 'Widgets', 'OpenGLExtensions', 'OpenGL', 'Network'])
sdl2_imagedep = dependency('SDL2_image')
sdl2dep = dependency('sdl2')
sqlite3dep = dependency('sqlite3')
threaddep = dependency('threads')
vadrmdep = dependency('libva-drm')
vax11dep = dependency('libva-x11')
x11dep = dependency('x11')
x264dep = dependency('x264')
zitaresamplerdep = cxx.find_library('zita-resampler')

# Use lld if we can; it links a lot faster than ld.bfd or gold.
code = '''#include <stdio.h>
int main() { printf("Hello, world!\n"); return 0; }
'''
if cxx.links(code, args: '-fuse-ld=lld', name: 'check for LLD')
	add_project_link_arguments('-fuse-ld=lld', language: 'cpp')
endif

# Add the right MOVIT_SHADER_DIR definition.
r = run_command('pkg-config', '--variable=shaderdir', 'movit')
if r.returncode() != 0
	error('Movit pkg-config installation is broken.')
endif
add_project_arguments('-DMOVIT_SHADER_DIR="' + r.stdout().strip() + '"', language: 'cpp')

# Make the Nageru version available as a #define.
add_project_arguments('-DNAGERU_VERSION="' + meson.project_version() + '"', language: 'cpp')

# DeckLink has these issues, and we include it from various places.
if cxx.has_argument('-Wno-non-virtual-dtor')
	add_project_arguments('-Wno-non-virtual-dtor', language: 'cpp')
endif

# FFmpeg has a lot of deprecated APIs whose replacements are not available
# in Debian stable, so we suppress these warnings.
if cxx.has_argument('-Wno-deprecated-declarations')
	add_project_arguments('-Wno-deprecated-declarations', language: 'cpp')
endif

# This needs to be done before declaring any build targets.
if get_option('cef_dir') != ''
	add_project_arguments('-DHAVE_CEF=1', language: 'cpp')
endif

top_include = include_directories('.')

subdir('shared')

# Nageru. (Not a subdir() because we don't want the output in nageru/nageru.)

nageru_srcs = []
nageru_deps = [shareddep, qt5deps, libjpegdep, movitdep, protobufdep,
	vax11dep, vadrmdep, x11dep, libavformatdep, libavresampledep, libavcodecdep, libavutildep,
	libswscaledep, libusbdep, luajitdep, dldep, x264dep, alsadep, zitaresamplerdep,
	qcustomplotdep, threaddep]
nageru_include_dirs = [include_directories('nageru')]
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
	# This is done in the top-level file; just kept here for reference.
	# add_project_arguments('-DHAVE_CEF=1', language: 'cpp')

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
	arguments : ['--proto_path=@CURRENT_SOURCE_DIR@/nageru', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process(['nageru/state.proto', 'nageru/midi_mapping.proto', 'nageru/json.proto'])
protobuf_lib = static_library('protobufs', proto_generated, dependencies: nageru_deps, include_directories: nageru_include_dirs)
protobuf_hdrs = declare_dependency(sources: proto_generated)
nageru_link_with += protobuf_lib

# Preprocess Qt as needed.
qt_files = qt5.preprocess(
	moc_headers: ['nageru/analyzer.h', 'nageru/clickable_label.h', 'nageru/compression_reduction_meter.h', 'nageru/correlation_meter.h',
		'nageru/ellipsis_label.h', 'nageru/glwidget.h', 'nageru/input_mapping_dialog.h', 'nageru/lrameter.h', 'nageru/mainwindow.h', 'nageru/midi_mapping_dialog.h',
		'nageru/nonlinear_fader.h', 'nageru/vumeter.h'],
	ui_files: ['nageru/analyzer.ui', 'nageru/audio_expanded_view.ui', 'nageru/audio_miniview.ui', 'nageru/display.ui',
		'nageru/input_mapping.ui', 'nageru/mainwindow.ui', 'nageru/midi_mapping.ui'],
	dependencies: qt5deps)

# Qt objects.
nageru_srcs += ['nageru/glwidget.cpp', 'nageru/mainwindow.cpp', 'nageru/vumeter.cpp', 'nageru/lrameter.cpp', 'nageru/compression_reduction_meter.cpp',
	'nageru/correlation_meter.cpp', 'nageru/analyzer.cpp', 'nageru/input_mapping_dialog.cpp', 'nageru/midi_mapping_dialog.cpp',
	'nageru/nonlinear_fader.cpp', 'nageru/context_menus.cpp', 'nageru/vu_common.cpp', 'nageru/piecewise_interpolator.cpp', 'nageru/midi_mapper.cpp']

# Auxiliary objects used for nearly everything.
aux_srcs = ['nageru/flags.cpp']
aux = static_library('aux', aux_srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += aux

# Audio objects.
audio_mixer_srcs = ['nageru/audio_mixer.cpp', 'nageru/alsa_input.cpp', 'nageru/alsa_pool.cpp', 'nageru/ebu_r128_proc.cc', 'nageru/stereocompressor.cpp',
	'nageru/resampling_queue.cpp', 'nageru/flags.cpp', 'nageru/correlation_measurer.cpp', 'nageru/filter.cpp', 'nageru/input_mapping.cpp']
audio = static_library('audio', audio_mixer_srcs, dependencies: [nageru_deps, protobuf_hdrs], include_directories: nageru_include_dirs)
nageru_link_with += audio

# Mixer objects.
nageru_srcs += ['nageru/chroma_subsampler.cpp', 'nageru/v210_converter.cpp', 'nageru/mixer.cpp', 'nageru/pbo_frame_allocator.cpp',
	'nageru/theme.cpp', 'nageru/image_input.cpp', 'nageru/alsa_output.cpp',
	'nageru/timecode_renderer.cpp', 'nageru/tweaked_inputs.cpp', 'nageru/mjpeg_encoder.cpp']

# Streaming and encoding objects (largely the set that is shared between Nageru and Kaeru).
stream_srcs = ['nageru/quicksync_encoder.cpp', 'nageru/x264_encoder.cpp', 'nageru/x264_dynamic.cpp', 'nageru/x264_speed_control.cpp', 'nageru/video_encoder.cpp',
	'nageru/audio_encoder.cpp', 'nageru/ffmpeg_util.cpp', 'nageru/ffmpeg_capture.cpp',
	'nageru/print_latency.cpp', 'nageru/basic_stats.cpp', 'nageru/ref_counted_frame.cpp']
stream = static_library('stream', stream_srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += stream

# DeckLink.
nageru_srcs += ['nageru/decklink_capture.cpp', 'nageru/decklink_util.cpp', 'nageru/decklink_output.cpp',
	'nageru/decklink/DeckLinkAPIDispatch.cpp']
decklink_dir = include_directories('nageru/decklink')
nageru_include_dirs += decklink_dir

# CEF input.
if have_cef
	nageru_srcs += ['nageru/nageru_cef_app.cpp', 'nageru/cef_capture.cpp']
endif

nageru_srcs += qt_files
nageru_srcs += proto_generated

# Shaders needed at runtime.
shaders = ['nageru/cbcr_subsample.vert', 'nageru/cbcr_subsample.frag', 'nageru/uyvy_subsample.vert', 'nageru/uyvy_subsample.frag', 'nageru/v210_subsample.comp', 'nageru/timecode.vert', 'nageru/timecode.frag', 'nageru/timecode_10bit.frag']
foreach shader : shaders
	run_command('ln', '-s', join_paths(meson.current_source_dir(), shader), meson.current_build_dir())
endforeach

nageru_shader_srcs = bin2h_gen.process(shaders)
nageru_srcs += nageru_shader_srcs

# Everything except main.cpp. (We do this because if you specify a .cpp file in
# both Nageru and Kaeru, it gets compiled twice. In the older Makefiles, Kaeru
# depended on a smaller set of objects.)
core = static_library('core', nageru_srcs, dependencies: nageru_deps, include_directories: nageru_include_dirs)
nageru_link_with += core

# Nageru executable; it goes into /usr/lib/nageru since CEF files go there, too
# (we can't put them straight into /usr/bin).
executable('nageru', 'nageru/main.cpp',
	dependencies: nageru_deps,
	include_directories: nageru_include_dirs,
	link_with: nageru_link_with,
	build_rpath: nageru_build_rpath,
	install_rpath: nageru_install_rpath,
	install: true,
	install_dir: exe_dir
)
meson.add_install_script('nageru/scripts/setup_nageru_symlink.sh')

# Kaeru executable.
executable('kaeru', 'nageru/kaeru.cpp',
	dependencies: [nageru_deps, kaeru_extra_deps],
	include_directories: nageru_include_dirs,
	link_with: [stream, aux, kaeru_link_with],
	install: true)

# Audio mixer microbenchmark.
executable('benchmark_audio_mixer', 'nageru/benchmark_audio_mixer.cpp', dependencies: nageru_deps, include_directories: nageru_include_dirs, link_with: [audio, aux])

# These are needed for a default run.
data_files = ['nageru/theme.lua', 'nageru/simple.lua', 'nageru/bg.jpeg', 'nageru/akai_midimix.midimapping']
install_data(data_files, install_dir: join_paths(get_option('prefix'), 'share/nageru'))
foreach file : data_files
	run_command('ln', '-s', join_paths(meson.current_source_dir(), file), meson.current_build_dir())
endforeach

# Futatabi. (Not a subdir() because we don't want the output in nageru/nageru.)

# Protobuf compilation.
gen = generator(protoc, \
	output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
	arguments : ['--proto_path=@CURRENT_SOURCE_DIR@/futatabi', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process('futatabi/state.proto', 'futatabi/frame.proto')

# Preprocess Qt as needed.
moc_files = qt5.preprocess(
	moc_headers: ['futatabi/mainwindow.h', 'futatabi/jpeg_frame_view.h', 'futatabi/clip_list.h'],
	ui_files: ['futatabi/mainwindow.ui'],
	dependencies: qt5deps)

# Flow objects.
futatabi_srcs = ['futatabi/flow.cpp', 'futatabi/gpu_timers.cpp']

# All the other files.
futatabi_srcs += ['futatabi/main.cpp', 'futatabi/player.cpp', 'futatabi/video_stream.cpp', 'futatabi/chroma_subsampler.cpp']
futatabi_srcs += ['futatabi/vaapi_jpeg_decoder.cpp', 'futatabi/db.cpp', 'futatabi/ycbcr_converter.cpp', 'futatabi/flags.cpp']
futatabi_srcs += ['futatabi/mainwindow.cpp', 'futatabi/jpeg_frame_view.cpp', 'futatabi/clip_list.cpp', 'futatabi/frame_on_disk.cpp']
futatabi_srcs += ['futatabi/export.cpp']
futatabi_srcs += moc_files
futatabi_srcs += proto_generated

# Shaders needed at runtime.
shaders = ['futatabi/chroma_subsample.vert', 'futatabi/densify.vert', 'futatabi/equations.vert', 'futatabi/hole_fill.vert', 'futatabi/motion_search.vert', 'futatabi/sor.vert', 'futatabi/splat.vert', 'futatabi/vs.vert']
shaders += ['futatabi/add_base_flow.frag', 'futatabi/blend.frag', 'futatabi/chroma_subsample.frag', 'futatabi/densify.frag', 'futatabi/derivatives.frag', 'futatabi/diffusivity.frag',
	'futatabi/equations.frag', 'futatabi/gray.frag', 'futatabi/hole_blend.frag', 'futatabi/hole_fill.frag', 'futatabi/motion_search.frag', 'futatabi/prewarp.frag', 'futatabi/resize_flow.frag',
	'futatabi/sobel.frag', 'futatabi/sor.frag', 'futatabi/splat.frag']

foreach shader : shaders
	run_command('ln', '-s', join_paths(meson.current_source_dir(), shader), meson.current_build_dir())
endforeach

futatabi_shader_srcs = bin2h_gen.process(shaders)
futatabi_srcs += futatabi_shader_srcs

executable('futatabi', futatabi_srcs,
	dependencies: [shareddep, qt5deps, libjpegdep, movitdep, libmicrohttpddep, protobufdep, sqlite3dep, vax11dep, vadrmdep, x11dep, libavformatdep, libavcodecdep, libavutildep, libswscaledep],
	link_with: shared,
	include_directories: [include_directories('futatabi')],
	install: true)

# Test binaries for the optical flow code.
executable('flow', 'futatabi/flow_main.cpp', 'futatabi/flow.cpp', 'futatabi/gpu_timers.cpp', futatabi_shader_srcs, dependencies: [shareddep, epoxydep, sdl2dep, sdl2_imagedep])
executable('eval', 'futatabi/eval.cpp', 'futatabi/util.cpp')
executable('vis', 'futatabi/vis.cpp', 'futatabi/util.cpp')
