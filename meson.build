project('nageru', 'cpp', default_options: ['buildtype=debugoptimized'])

cxx = meson.get_compiler('cpp')

# Use lld if we can; it links a lot faster than ld.bfd or gold.
code = '''#include <stdio.h>
int main() { printf("Hello, world!\n"); return 0; }
'''
if cxx.links(code, args: '-fuse-ld=lld', name: 'check for LLD')
	add_project_link_arguments('-fuse-ld=lld')
endif

# Add the right MOVIT_SHADER_DIR definition.
r = run_command('pkg-config', '--variable=shaderdir', 'movit')
if r.returncode() != 0
	error('Movit pkg-config installation is broken.')
endif
add_project_arguments('-DMOVIT_SHADER_DIR="' + r.stdout().strip() + '"', language: 'cpp')

# DeckLink has these issues, and we include it from various places.
if cxx.has_argument('-Wno-non-virtual-dtor')
	add_project_arguments('-Wno-non-virtual-dtor', language: 'cpp')
endif

# FFmpeg has a lot of deprecated APIs whose replacements are not available
# in Debian stable, so we suppress these warnings.
if cxx.has_argument('-Wno-deprecated-declarations')
	add_project_arguments('-Wno-deprecated-declarations', language: 'cpp')
endif

top_include = include_directories('.')

subdir('shared')
subdir('nageru')
subdir('futatabi')
