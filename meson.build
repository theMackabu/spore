project('spore', 'c', default_options: [
  'c_std=c23',
  'build.c_std=c23',
  'warning_level=3',
  'b_asneeded=false',
  'b_pie=false',
], version: '0.7.0')

spore_sysname = 'Spore'
spore_nodename = 'spore.local'
spore_machine = 'aarch64'
spore_domainname = 'Spore'

spore_release = meson.project_version()
spore_build_date = run_command('date', '+%a %b %d %Y', check: true).stdout().strip()
spore_uts_version = spore_sysname + ' Kernel Version ' + spore_release + ': ' + spore_build_date + ';'

subdir('userland')
subdir('tools')
subdir('kernel')
subdir('tests')
