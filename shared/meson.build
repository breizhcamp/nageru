qt5 = import('qt5')
qt5deps = dependency('qt5', modules: ['OpenGL'])

srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp', 'context.cpp']
shared = static_library('shared', srcs, include_directories: top_include, dependencies: qt5deps)
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared)
