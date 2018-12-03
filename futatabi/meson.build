qt5 = import('qt5')
protoc = find_program('protoc')

epoxydep = dependency('epoxy')
libavcodecdep = dependency('libavcodec')
libavformatdep = dependency('libavformat')
libavutildep = dependency('libavutil')
libjpegdep = dependency('libjpeg')
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

# Protobuf compilation.
gen = generator(protoc, \
  output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process('state.proto', 'frame.proto')

# Preprocess Qt as needed.
moc_files = qt5.preprocess(
  moc_headers: ['mainwindow.h', 'jpeg_frame_view.h', 'clip_list.h'],
  ui_files: ['mainwindow.ui'],
  dependencies: qt5deps)

# Flow objects.
srcs = ['flow.cpp', 'gpu_timers.cpp']

# All the other files.
srcs += ['main.cpp', 'player.cpp', 'video_stream.cpp', 'chroma_subsampler.cpp']
srcs += ['vaapi_jpeg_decoder.cpp', 'db.cpp', 'disk_space_estimator.cpp', 'ycbcr_converter.cpp', 'flags.cpp']
srcs += ['mainwindow.cpp', 'jpeg_frame_view.cpp', 'clip_list.cpp', 'frame_on_disk.cpp']
srcs += moc_files
srcs += proto_generated

# Shaders needed at runtime.
shaders = ['chroma_subsample.vert', 'densify.vert', 'equations.vert', 'hole_fill.vert', 'motion_search.vert', 'sor.vert', 'splat.vert', 'vs.vert']
shaders += ['add_base_flow.frag', 'blend.frag', 'chroma_subsample.frag', 'densify.frag', 'derivatives.frag', 'diffusivity.frag',
  'equations.frag', 'gray.frag', 'hole_blend.frag', 'hole_fill.frag', 'motion_search.frag', 'prewarp.frag', 'resize_flow.frag',
  'sobel.frag', 'sor.frag', 'splat.frag']

foreach shader : shaders
  run_command('ln', '-s', join_paths(meson.current_source_dir(), shader), meson.current_build_dir())
endforeach

bin2h = executable('bin2h', 'bin2h.cpp')
bin2h_gen = generator(bin2h, \
  output    : ['@PLAINNAME@.cpp'],
  arguments : ['@INPUT@', '@PLAINNAME@', '@OUTPUT@'])
shader_srcs = bin2h_gen.process(shaders)
srcs += shader_srcs

executable('futatabi', srcs, dependencies: [shareddep, qt5deps, libjpegdep, movitdep, libmicrohttpddep, protobufdep, sqlite3dep, vax11dep, vadrmdep, x11dep, libavformatdep, libavcodecdep, libavutildep, libswscaledep], link_with: shared)
executable('flow', 'flow_main.cpp', 'flow.cpp', 'gpu_timers.cpp', shader_srcs, dependencies: [epoxydep, sdl2dep, sdl2_imagedep])
executable('eval', 'eval.cpp', 'util.cpp')
executable('vis', 'vis.cpp', 'util.cpp')
