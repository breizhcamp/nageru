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

# Protobuf compilation.
gen = generator(protoc, \
        output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
        arguments : ['--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])
proto_generated = gen.process(['midi_mapping.proto'])
protobuf_lib = static_library('protobufs', proto_generated, dependencies: [protobufdep])
protobuf_hdrs = declare_dependency(sources: proto_generated)

srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp', 'context.cpp', 'httpd.cpp', 'disk_space_estimator.cpp', 'read_file.cpp', 'text_proto.cpp', 'midi_device.cpp']
srcs += proto_generated

# Qt objects.
srcs += qt_files
srcs += ['aboutdialog.cpp']

shared = static_library('shared', srcs, include_directories: top_include, dependencies: [shared_qt5deps, libmicrohttpddep, protobufdep, alsadep])
shareddep = declare_dependency(
   sources: proto_generated,
   include_directories: top_include,
   link_with: [shared, protobuf_lib])

bin2h = executable('bin2h', 'bin2h.cpp')
bin2h_gen = generator(bin2h, \
  output    : ['@PLAINNAME@.cpp'],
  arguments : ['@INPUT@', '@PLAINNAME@', '@OUTPUT@'])
