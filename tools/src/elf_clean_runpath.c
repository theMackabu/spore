#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  EI_CLASS = 4,
  EI_DATA = 5,
  ELFCLASS64 = 2,
  ELFDATA2LSB = 1,
  PT_DYNAMIC = 2,
  DT_NULL = 0,
  DT_RPATH = 15,
  DT_RUNPATH = 29,
};

constexpr unsigned char ELFMAG[] = {0x7f, 'E', 'L', 'F'};

typedef struct {
  unsigned char e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
} Elf64_Phdr;

typedef struct {
  int64_t d_tag;
  union {
    uint64_t d_val;
    uint64_t d_ptr;
  } d_un;
} Elf64_Dyn;

static bool read_at(FILE *file, long off, void *buf, size_t len) {
  return fseek(file, off, SEEK_SET) == 0 && fread(buf, 1, len, file) == len;
}

static bool write_at(FILE *file, long off, const void *buf, size_t len) {
  return fseek(file, off, SEEK_SET) == 0 && fwrite(buf, 1, len, file) == len;
}

static int clean_one(const char *path) {
  FILE *file = fopen(path, "r+b");
  if (file == NULL) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }

  Elf64_Ehdr eh;
  if (!read_at(file, 0, &eh, sizeof(eh)) || memcmp(eh.e_ident, ELFMAG, sizeof(ELFMAG)) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
    fprintf(stderr, "%s: not a supported ELF64 little-endian file\n", path);
    fclose(file);
    return 1;
  }

  bool changed = false;
  for (uint16_t i = 0; i < eh.e_phnum; ++i) {
    Elf64_Phdr ph;
    long ph_off = (long)eh.e_phoff + (long)i * (long)eh.e_phentsize;
    if (!read_at(file, ph_off, &ph, sizeof(ph))) {
      fprintf(stderr, "%s: failed to read program header\n", path);
      fclose(file);
      return 1;
    }
    if (ph.p_type != PT_DYNAMIC || ph.p_filesz < sizeof(Elf64_Dyn)) { continue; }

    size_t count = (size_t)(ph.p_filesz / sizeof(Elf64_Dyn));
    Elf64_Dyn *dyn = calloc(count, sizeof(*dyn));
    if (dyn == NULL) {
      fprintf(stderr, "%s: out of memory\n", path);
      fclose(file);
      return 1;
    }
    if (!read_at(file, (long)ph.p_offset, dyn, count * sizeof(*dyn))) {
      fprintf(stderr, "%s: failed to read dynamic table\n", path);
      free(dyn);
      fclose(file);
      return 1;
    }

    for (size_t j = 0; j < count;) {
      if (dyn[j].d_tag != DT_RPATH && dyn[j].d_tag != DT_RUNPATH) {
        ++j;
        continue;
      }
      memmove(&dyn[j], &dyn[j + 1], (count - j - 1) * sizeof(*dyn));
      dyn[count - 1].d_tag = DT_NULL;
      dyn[count - 1].d_un.d_val = 0;
      changed = true;
    }

    if (changed && !write_at(file, (long)ph.p_offset, dyn, count * sizeof(*dyn))) {
      fprintf(stderr, "%s: failed to write dynamic table\n", path);
      free(dyn);
      fclose(file);
      return 1;
    }
    free(dyn);
  }

  if (fclose(file) != 0) {
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: elf-clean-runpath ELF...\n");
    return 2;
  }
  int rc = 0;
  for (int i = 1; i < argc; ++i) {
    if (clean_one(argv[i]) != 0) { rc = 1; }
  }
  return rc;
}
