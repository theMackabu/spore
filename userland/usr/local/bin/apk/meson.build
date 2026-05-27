apk_patch_files = files(
  'patches/0001-use-fork-for-url-fetcher.patch',
  'patches/0002-wget-skip-empty-timeout.patch',
  'patches/0003-libfetch-accept-mbedtls-dependency-list.patch',
  'patches/0004-wget-fetch-to-tempfile.patch',
  'patches/0005-wget-unlink-tempfile-on-close.patch',
)

userland_apk = custom_target('userland-apk',
  output: 'apk',
  command: [
    files('build.sh'),
    meson.project_source_root(),
    meson.current_build_dir(),
    meson.project_build_root(),
    '@OUTPUT@',
  ],
  depend_files: apk_patch_files,
  depends: [userland_zlib])
userland_image_targets += userland_apk
