mbedtls_install = meson.project_build_root() / 'userland/lib/mbedtls/mbedtls-install'
mbedtls_include = mbedtls_install / 'include'

userland_trustd = custom_target('userland-trustd',
  input: files('main.c'),
  output: 'trustd',
  command: userland_static_cc + [
    '-I' + mbedtls_include,
    '@INPUT@',
    mbedtls_install / 'lib/libmbedtls.a',
    mbedtls_install / 'lib/libmbedx509.a',
    mbedtls_install / 'lib/libmbedcrypto.a',
    '-o', '@OUTPUT@',
  ],
  depends: userland_mbedtls)
userland_image_targets += userland_trustd
