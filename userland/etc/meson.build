os_release_conf = configuration_data()
os_release_conf.set('SPORE_RELEASE', spore_release)
userland_os_release = configure_file(
  input: 'os-release.in',
  output: 'os-release',
  configuration: os_release_conf)

userland_image_data += files(
  'motd',
  'update-motd.d/90-last-login',
  'environment',
  'profile',
  'dashrc',
  'mycelium/boot.target',
  'mycelium/multi-user.target',
  'mycelium/shutdown.target',
  'mycelium/console.service',
  'mycelium/hostname.service',
  'mycelium/network.service',
  'mycelium/network.target',
  'hostname',
  'hosts',
  'resolv.conf',
  'fastfetch/config.jsonc',
  'fastfetch/spore.logo',
) + userland_os_release
