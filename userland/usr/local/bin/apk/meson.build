apk_patch_files = files(
  'patches/0003-stream-url-fetches-with-curl.patch',
  'patches/0004-spm-brand-and-install-alias.patch',
  'patches/0005-abort-on-extract-error.patch',
)

userland_apk = custom_target('userland-apk',
  output: 'spm',
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
