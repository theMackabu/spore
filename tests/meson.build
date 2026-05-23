test_include = [
  include_directories('../kernel/src/include'),
  include_directories('../third_party/limine-protocol/include'),
]

test_pmm = executable('test_pmm',
  files(
    'test_pmm.c',
    '../kernel/src/mm/pmm.c',
    '../kernel/src/lib/mem.c',
  ),
  include_directories: test_include,
  native: true)

test('pmm', test_pmm)

test_stack = executable('test_stack',
  files(
    'test_stack.c',
    '../kernel/src/exec/stack.c',
    '../kernel/src/lib/mem.c',
  ),
  include_directories: test_include,
  native: true)

test('stack', test_stack)

test_elf = executable('test_elf',
  files(
    'test_elf.c',
    '../kernel/src/elf/loader.c',
    '../kernel/src/lib/mem.c',
  ),
  include_directories: test_include,
  native: true)

test('elf', test_elf)

test_ramfs = executable('test_ramfs',
  files(
    'test_ramfs.c',
    '../kernel/src/ramfs.c',
    '../kernel/src/lib/mem.c',
  ),
  include_directories: test_include,
  native: true)

test('ramfs', test_ramfs)

test_vmm = executable('test_vmm',
  files(
    'test_vmm.c',
    '../kernel/src/mm/vmm.c',
    '../kernel/src/lib/mem.c',
  ),
  include_directories: test_include,
  native: true)

test('vmm', test_vmm)
