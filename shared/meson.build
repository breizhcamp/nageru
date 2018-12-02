
srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp', 'mux.cpp', 'metrics.cpp']
shared = static_library('shared', srcs, include_directories: top_include)
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared)
