qt5 = import('qt5')
qt5deps = dependency('qt5', modules: ['OpenGL'])
libmicrohttpddep = dependency('libmicrohttpd')

srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp', 'context.cpp', 'httpd.cpp', 'disk_space_estimator.cpp', 'read_file.cpp']
shared = static_library('shared', srcs, include_directories: top_include, dependencies: [qt5deps, libmicrohttpddep])
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared)

bin2h = executable('bin2h', 'bin2h.cpp')
bin2h_gen = generator(bin2h, \
  output    : ['@PLAINNAME@.cpp'],
  arguments : ['@INPUT@', '@PLAINNAME@', '@OUTPUT@'])
