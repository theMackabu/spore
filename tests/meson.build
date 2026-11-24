host_kernel_rust = custom_target('host-kernel-rust',
  input: kernel_rust_root,
  output: 'libhost_kernel_rust.a',
  depfile: 'host_kernel_rust.d',
  command: [rustc, rust_archive_args, '--crate-name=host_kernel_rust',
    '--emit=link,dep-info=@DEPFILE@', '@INPUT@', '-o', '@OUTPUT@'])

test_include = [
  include_directories('../kernel/include'),
]

host_mem = static_library('host-kernel-mem', memory_primitives + table_sources,
  include_directories: test_include,
  native: true)

test_pmm = executable('test_pmm',
  pmm_sources, 'test_pmm.c',
  link_with: host_mem,
  include_directories: test_include,
  native: true)

test('pmm', test_pmm)

test_stack = executable('test_stack',
  stack_sources, 'test_stack.c', 'random_stub.c',
  link_with: host_mem,
  include_directories: test_include,
  native: true)

test('stack', test_stack)

test_elf = executable('test_elf',
  elf_sources + vma_sources, 'test_elf.c', 'vfs_ref_stub.c',
  link_with: host_mem,
  link_args: host_kernel_rust.full_path(),
  link_depends: host_kernel_rust,
  include_directories: test_include,
  native: true)

test('elf', test_elf)

test_ramfs = executable('test_ramfs',
  ramfs_sources, 'test_ramfs.c',
  link_with: host_mem,
  include_directories: test_include,
  native: true)

test('ramfs', test_ramfs)

host_ext2 = static_library('host-ext2', ext2_sources,
  include_directories: test_include,
  native: true)

test_ext2 = executable('test_ext2', 'test_ext2.c',
  link_with: [host_ext2, host_mem],
  link_args: host_kernel_rust.full_path(),
  link_depends: host_kernel_rust,
  include_directories: test_include,
  native: true)

test('ext2', test_ext2, args: [meson.project_build_root() / 'root.ext2'])

bench_ext2_inodes = executable('bench_ext2_inodes', 'bench_ext2_inodes.c',
  link_with: [host_ext2, host_mem],
  link_args: host_kernel_rust.full_path(),
  link_depends: host_kernel_rust,
  include_directories: test_include,
  native: true)

test_vmm = executable('test_vmm',
  vmm_sources + aarch64_mmu_sources, 'test_vmm.c',
  link_with: host_mem,
  include_directories: test_include,
  native: true)

test('vmm', test_vmm)

test_vma = executable('test_vma',
  vma_sources, 'test_vma.c', 'vfs_ref_stub.c',
  link_with: host_mem,
  include_directories: test_include,
  native: true)

test('vma', test_vma)

test_abi = executable('test_abi',
  files('test_abi.c'),
  include_directories: test_include,
  native: true)

test('abi', test_abi)

test_thread_model = executable('test_thread_model',
  files('test_thread_model.c'),
  include_directories: test_include,
  native: true)

test('thread-model', test_thread_model)

test_futex_model = executable('test_futex_model',
  files('test_futex_model.c'),
  include_directories: test_include,
  native: true)

test('futex-model', test_futex_model)

test_ipc_model = executable('test_ipc_model',
  files('test_ipc_model.c'),
  include_directories: test_include,
  native: true)

test('ipc-model', test_ipc_model)

test_mycelium_model = executable('test_mycelium_model',
  files('test_mycelium_model.c'),
  include_directories: test_include,
  native: true)

test('mycelium-model', test_mycelium_model)

test_net = executable('test_net',
  network_sources, 'test_net.c',
  link_with: host_mem,
  link_args: host_kernel_rust.full_path(),
  link_depends: host_kernel_rust,
  include_directories: test_include,
  native: true)

test('net', test_net)
