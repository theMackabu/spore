spore_image = executable('spore-image', 'src/spore_image.rs', native: true, override_options: ['rust_std=2024'], install: false)

runner_terminal = static_library('runner-terminal', 'src/runner/terminal.c', native: true)
spore_run = executable('spore-run', 'src/spore_run.rs',
  link_with: runner_terminal,
  native: true, override_options: ['rust_std=2024'], install: false)
