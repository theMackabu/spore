python = import('python').find_installation('python3')
manifest = files('manifest.py', 'packages.json')
cache_dir = meson.project_source_root() / '.cache/packages'

packages = run_command(python, manifest, 'packages', check: true).stdout().strip()
foreach line : packages.split('\n')
  fields = line.split('\t')
  members = fields.slice(2, fields.length())
  outputs = []
  foreach member : members
    outputs += member.split('/')[-1]
  endforeach
  package = custom_target('userland-' + outputs[0],
    output: outputs,
    command: [python, files('unpack.py'), cache_dir, fields[0], fields[1], meson.current_build_dir(), members])
  userland_image_targets += package
  if outputs.contains('ant')
    ant_prebuilt = package
  endif
endforeach

if not get_option('userland')
  subdir_done()
endif

sdks = {}
sdk_inputs = run_command(python, manifest, 'sdks', check: true).stdout().strip()
foreach line : sdk_inputs.split('\n')
  fields = line.split('\t')
  name = fields[0]
  sdks += {name: custom_target(name + '-sdk',
    output: name + '-sdk.stamp',
    command: [python, '-B', files('sdk.py'), cache_dir, meson.current_build_dir() / (name + '-sdk'),
              '@OUTPUT@', fields.slice(1, fields.length())],
    depend_files: files('unpack.py'))}
endforeach
