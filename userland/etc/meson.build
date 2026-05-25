os_release_conf = configuration_data()
os_release_conf.set('SPORE_RELEASE', spore_release)
userland_os_release = configure_file(
  input: 'os-release.in',
  output: 'os-release',
  configuration: os_release_conf)

userland_image_data += files(
  'motd',
  'environment',
  'profile',
  'dashrc',
  'mycelium/boot.target',
  'mycelium/multi-user.target',
  'mycelium/shutdown.target',
  'mycelium/console.service',
  'fastfetch/config.jsonc',
  'fastfetch/spore.logo',
) + userland_os_release
