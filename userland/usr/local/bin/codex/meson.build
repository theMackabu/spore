codex_version = '0.147.0'
codex_archive_sha256 = 'eb677c80f666b1ab8b4b1d083b66e8d614b1281d960bb6f9fd8ca98f58b38b90'
codex_archive_url = 'https://github.com/openai/codex/releases/download/rust-v' + codex_version + '/codex-aarch64-unknown-linux-musl.tar.gz'

userland_codex = custom_target('userland-codex',
  output: 'codex',
  command: [
    find_program('python3'),
    meson.project_source_root() / 'tools/fetch_codex.py',
    codex_archive_url,
    codex_archive_sha256,
    'codex-aarch64-unknown-linux-musl',
    '@OUTPUT@',
  ])
userland_image_targets += userland_codex
