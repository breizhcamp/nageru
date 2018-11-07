project('futatabi', 'cpp')
qt5 = import('qt5')
protoc = find_program('protoc')

epoxydep = dependency('epoxy')
libavcodecdep = dependency('libavcodec')
libavformatdep = dependency('libavformat')
libavutildep = dependency('libavutil')
libjpegdep = dependency('libjpeg')
libmicrohttpddep = dependency('libmicrohttpd')
libswscaledep = dependency('libswscale')
movitdep = dependency('movit')
protobufdep = dependency('protobuf')
qt5deps = dependency('qt5', modules: ['Core', 'Gui', 'Widgets', 'OpenGLExtensions', 'OpenGL', 'PrintSupport'])
sdl2_imagedep = dependency('SDL2_image')
sdl2dep = dependency('sdl2')
sqlite3dep = dependency('sqlite3')
vadrmdep = dependency('libva-drm')
vax11dep = dependency('libva-x11')
x11dep = dependency('x11')

# Add the right MOVIT_SHADER_DIR definition.
r = run_command('pkg-config', '--variable=shaderdir', 'movit')
if r.returncode() != 0
  error('Movit pkg-config installation is broken.')
endif
add_global_arguments('-DMOVIT_SHADER_DIR="' + r.stdout().strip() + '"', language: 'cpp')

# Protobuf compilation.
gen = generator(protoc, \
  output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process('state.proto')

# Preprocess Qt as needed.
moc_files = qt5.preprocess(
  moc_headers: ['mainwindow.h', 'jpeg_frame_view.h', 'clip_list.h'],
  ui_files: ['mainwindow.ui'],
  dependencies: qt5deps)

# Flow objects.
srcs = ['flow.cpp', 'gpu_timers.cpp']

# All the other files.
srcs += ['ffmpeg_raii.cpp', 'main.cpp', 'player.cpp', 'httpd.cpp', 'mux.cpp', 'metacube2.cpp', 'video_stream.cpp', 'context.cpp', 'chroma_subsampler.cpp']
srcs += ['vaapi_jpeg_decoder.cpp', 'memcpy_interleaved.cpp', 'db.cpp', 'disk_space_estimator.cpp', 'ycbcr_converter.cpp', 'flags.cpp']
srcs += ['mainwindow.cpp', 'jpeg_frame_view.cpp', 'clip_list.cpp']
srcs += moc_files
srcs += proto_generated

executable('futatabi', srcs, dependencies: [qt5deps, libjpegdep, movitdep, libmicrohttpddep, protobufdep, sqlite3dep, vax11dep, vadrmdep, x11dep, libavformatdep, libavcodecdep, libavutildep, libswscaledep])
executable('flow', 'flow_main.cpp', 'flow.cpp', 'gpu_timers.cpp', dependencies: [epoxydep, sdl2dep, sdl2_imagedep])
executable('eval', 'eval.cpp', 'util.cpp')
executable('vis', 'vis.cpp', 'util.cpp')
