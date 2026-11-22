spore_image = executable('spore-image',
  files('src/spore_image.c'),
  native: true,
  install: false)

spore_run = executable('spore-run',
  files(
    'src/spore_run.c',
    'src/runner/harness.c',
    'src/runner/fixture_servers.c',
    'src/runner/guest_checks.c',
    'src/runner/shell_check.c',
    'src/runner/bench.c',
    'src/runner/rng.c',
  ),
  native: true,
  install: false)
