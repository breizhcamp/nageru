
srcs = ['memcpy_interleaved.cpp', 'metacube2.cpp', 'ffmpeg_raii.cpp']
shared = static_library('shared', srcs, include_directories: top_include)
shareddep = declare_dependency(
   include_directories: top_include,
   link_with: shared,
   sources: srcs)
