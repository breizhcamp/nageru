project('nageru', 'cpp', default_options: ['buildtype=debugoptimized'])

# Add the right MOVIT_SHADER_DIR definition.
r = run_command('pkg-config', '--variable=shaderdir', 'movit')
if r.returncode() != 0
	error('Movit pkg-config installation is broken.')
endif
add_project_arguments('-DMOVIT_SHADER_DIR="' + r.stdout().strip() + '"', language: 'cpp')

subdir('nageru')
subdir('futatabi')
