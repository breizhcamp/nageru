qt5 = import('qt5')
shared_qt5deps = dependency('qt5', modules: ['Core', 'Gui', 'Widgets', 'OpenGL'])
libmicrohttpddep = dependency('libmicrohttpd')
protobufdep = dependency('protobuf')
alsadep = dependency('alsa')

# Preprocess Qt as needed.
qt_files = qt5.preprocess(
	moc_headers: ['aboutdialog.h'],
	ui_files: ['aboutdialog.ui'],
	dependencies: shared_qt5deps)

srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp', 'context.cpp', 'httpd.cpp', 'disk_space_estimator.cpp', 'read_file.cpp', 'text_proto.cpp', 'midi_device.cpp']

# Qt objects.
srcs += qt_files
srcs += ['aboutdialog.cpp']

shared = static_library('shared', srcs, include_directories: top_include, dependencies: [shared_qt5deps, libmicrohttpddep, protobufdep, alsadep])
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared)

bin2h = executable('bin2h', 'bin2h.cpp')
bin2h_gen = generator(bin2h, \
  output    : ['@PLAINNAME@.cpp'],
  arguments : ['@INPUT@', '@PLAINNAME@', '@OUTPUT@'])
