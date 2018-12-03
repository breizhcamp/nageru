qt5 = import('qt5')
qt5deps = dependency('qt5', modules: ['OpenGL'])
libmicrohttpddep = dependency('libmicrohttpd')

srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp', 'context.cpp', 'httpd.cpp', 'disk_space_estimator.cpp']
shared = static_library('shared', srcs, include_directories: top_include, dependencies: [qt5deps, libmicrohttpddep])
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared)
