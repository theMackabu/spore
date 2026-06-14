#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_GLYPHS = 1024,
  MAX_FONT_HEIGHT = 128,
  MAX_BITMAP_ROWS = 256,
};

struct glyph {
  uint32_t codepoint;
  uint16_t rows[MAX_FONT_HEIGHT];
};

struct glyph_set {
  struct glyph glyphs[MAX_GLYPHS];
  size_t count;
};

static bool starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool wanted_codepoint(int codepoint) {
  if (codepoint >= 0x20 && codepoint < 0x7f) { return true; }
  if (codepoint >= 0xa0 && codepoint < 0x100) { return true; }
  if (codepoint >= 0x2500 && codepoint < 0x25a0) { return true; }

  static const int extra[] = {
    0x203a, 0x2190, 0x2191, 0x2192, 0x2193, 0x21b5, 0x2219, 0x221a, 0x221e, 0x2260,
    0x2261, 0x2264, 0x2265, 0x2320, 0x2321, 0x23ba, 0x23bb, 0x23bc, 0x23bd, 0x276f,
    0xfffd,
  };
  for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
    if (codepoint == extra[i]) { return true; }
  }
  return false;
}

static struct glyph *find_glyph(struct glyph_set *set, uint32_t codepoint) {
  for (size_t i = 0; i < set->count; ++i) {
    if (set->glyphs[i].codepoint == codepoint) { return &set->glyphs[i]; }
  }
  return NULL;
}

static struct glyph *upsert_glyph(struct glyph_set *set, uint32_t codepoint) {
  struct glyph *glyph = find_glyph(set, codepoint);
  if (glyph != NULL) { return glyph; }
  if (set->count >= MAX_GLYPHS) {
    fprintf(stderr, "bdf_to_kernel_font: too many selected glyphs\n");
    exit(1);
  }
  glyph = &set->glyphs[set->count++];
  memset(glyph, 0, sizeof(*glyph));
  glyph->codepoint = codepoint;
  return glyph;
}

static void clone_glyph_if_missing(struct glyph_set *set, uint32_t dst_codepoint, uint32_t src_codepoint) {
  if (find_glyph(set, dst_codepoint) != NULL) { return; }
  struct glyph *src = find_glyph(set, src_codepoint);
  if (src == NULL) { return; }
  struct glyph *dst = upsert_glyph(set, dst_codepoint);
  memcpy(dst->rows, src->rows, sizeof(dst->rows));
}

static int compare_glyphs(const void *a, const void *b) {
  const struct glyph *ga = a;
  const struct glyph *gb = b;
  if (ga->codepoint < gb->codepoint) { return -1; }
  if (ga->codepoint > gb->codepoint) { return 1; }
  return 0;
}

static void strip_newline(char *line) {
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
}

static void normalize_bitmap(struct glyph *glyph, const uint32_t *bitmap, size_t bitmap_count, int bbx_width,
                             int bbx_height, int xoff, int yoff, int font_width, int font_height, int font_xoff,
                             int font_yoff) {
  memset(glyph->rows, 0, sizeof(glyph->rows));
  if (bbx_width <= 0 || bbx_height <= 0) {
    bbx_width = font_width;
    bbx_height = font_height;
    xoff = 0;
    yoff = 0;
  }
  if (bbx_width > 32) {
    fprintf(stderr, "bdf_to_kernel_font: glyph width %d exceeds parser storage\n", bbx_width);
    exit(1);
  }

  int top = (font_height + font_yoff) - (bbx_height + yoff);
  size_t rows = bitmap_count < (size_t)bbx_height ? bitmap_count : (size_t)bbx_height;
  for (size_t src_row = 0; src_row < rows; ++src_row) {
    int dst_row = top + (int)src_row;
    if (dst_row < 0 || dst_row >= font_height) { continue; }

    int src_shift = ((bbx_width + 7) & ~7) - bbx_width;
    uint32_t mask = bbx_width == 32 ? UINT32_MAX : ((1u << bbx_width) - 1u);
    uint32_t bits = (bitmap[src_row] >> src_shift) & mask;
    int row_width = bbx_width;
    int left = xoff - font_xoff;
    if (left < 0) { left = 0; }
    if (left >= font_width) { continue; }
    if (left + row_width > font_width) {
      bits >>= left + row_width - font_width;
      row_width = font_width - left;
    }
    (void)row_width;
    glyph->rows[dst_row] |= (uint16_t)(bits << (16 - font_width + left));
  }
}

static void parse_bdf(const char *path, int *width_out, int *height_out, struct glyph_set *set) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    perror(path);
    exit(1);
  }

  char line[512];
  int font_width = 0;
  int font_height = 0;
  int font_xoff = 0;
  int font_yoff = 0;
  int encoding = -1;
  int bbx_width = 0;
  int bbx_height = 0;
  int xoff = 0;
  int yoff = 0;
  bool have_bbx = false;
  bool in_bitmap = false;
  uint32_t bitmap[MAX_BITMAP_ROWS];
  size_t bitmap_count = 0;

  while (fgets(line, sizeof(line), file) != NULL) {
    strip_newline(line);
    if (starts_with(line, "FONTBOUNDINGBOX ")) {
      if (sscanf(line, "FONTBOUNDINGBOX %d %d %d %d", &font_width, &font_height, &font_xoff, &font_yoff) != 4) {
        fprintf(stderr, "%s: malformed FONTBOUNDINGBOX\n", path);
        exit(1);
      }
      if (font_width > 16) {
        fprintf(stderr, "%s: font width %d exceeds uint16 row storage\n", path, font_width);
        exit(1);
      }
      if (font_height <= 0 || font_height > MAX_FONT_HEIGHT) {
        fprintf(stderr, "%s: font height %d exceeds generator storage\n", path, font_height);
        exit(1);
      }
    } else if (starts_with(line, "STARTCHAR ")) {
      encoding = -1;
      have_bbx = false;
      in_bitmap = false;
      bitmap_count = 0;
    } else if (starts_with(line, "ENCODING ")) {
      if (sscanf(line, "ENCODING %d", &encoding) != 1) {
        fprintf(stderr, "%s: malformed ENCODING\n", path);
        exit(1);
      }
    } else if (starts_with(line, "BBX ")) {
      if (sscanf(line, "BBX %d %d %d %d", &bbx_width, &bbx_height, &xoff, &yoff) != 4) {
        fprintf(stderr, "%s: malformed BBX\n", path);
        exit(1);
      }
      have_bbx = true;
    } else if (strcmp(line, "BITMAP") == 0) {
      in_bitmap = true;
      bitmap_count = 0;
    } else if (strcmp(line, "ENDCHAR") == 0) {
      if (wanted_codepoint(encoding) && in_bitmap) {
        struct glyph *glyph = upsert_glyph(set, (uint32_t)encoding);
        normalize_bitmap(glyph, bitmap, bitmap_count, have_bbx ? bbx_width : 0, have_bbx ? bbx_height : 0,
                         have_bbx ? xoff : 0, have_bbx ? yoff : 0, font_width, font_height, font_xoff, font_yoff);
      }
      encoding = -1;
      have_bbx = false;
      in_bitmap = false;
      bitmap_count = 0;
    } else if (in_bitmap) {
      if (bitmap_count >= MAX_BITMAP_ROWS) {
        fprintf(stderr, "%s: glyph bitmap exceeds generator storage\n", path);
        exit(1);
      }
      bitmap[bitmap_count++] = (uint32_t)strtoul(line, NULL, 16);
    }
  }
  fclose(file);

  if (font_width == 0 || font_height == 0) {
    fprintf(stderr, "%s: missing FONTBOUNDINGBOX\n", path);
    exit(1);
  }
  if (find_glyph(set, 0xfffd) == NULL) {
    struct glyph *question = find_glyph(set, '?');
    if (question != NULL) {
      struct glyph *replacement = upsert_glyph(set, 0xfffd);
      memcpy(replacement->rows, question->rows, sizeof(replacement->rows));
    }
  }
  clone_glyph_if_missing(set, 0x276f, 0x203a);
  qsort(set->glyphs, set->count, sizeof(set->glyphs[0]), compare_glyphs);
  *width_out = font_width;
  *height_out = font_height;
}

static const char *basename_of(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash == NULL ? path : slash + 1;
}

static void write_header(const char *path, int width, int height) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    perror(path);
    exit(1);
  }
  fprintf(file,
          "#pragma once\n"
          "\n"
          "#include <stdint.h>\n"
          "\n"
          "enum {\n"
          "  TERMINUS_FONT_WIDTH = %d,\n"
          "  TERMINUS_FONT_HEIGHT = %d,\n"
          "};\n"
          "\n"
          "struct terminus_glyph {\n"
          "  uint32_t codepoint;\n"
          "  uint16_t rows[TERMINUS_FONT_HEIGHT];\n"
          "};\n"
          "\n"
          "extern const struct terminus_glyph terminus_font_glyphs[];\n"
          "extern const unsigned terminus_font_glyph_count;\n",
          width, height);
  fclose(file);
}

static void write_source(const char *path, const char *header_name, const struct glyph_set *set, int height) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    perror(path);
    exit(1);
  }
  fprintf(file,
          "/* Generated from Terminus Font 4.49.1 ter-u16b.bdf.\n"
          " * Terminus is Copyright (C) 2020 Dimitar Toshkov Zhekov\n"
          " * and licensed under the SIL Open Font License, Version 1.1.\n"
          " */\n"
          "#include \"%s\"\n"
          "\n"
          "const struct terminus_glyph terminus_font_glyphs[] = {\n",
          header_name);
  for (size_t i = 0; i < set->count; ++i) {
    fprintf(file, "  {0x%04x, {", set->glyphs[i].codepoint);
    for (int row = 0; row < height; ++row) {
      fprintf(file, "%s0x%04x", row == 0 ? "" : ", ", set->glyphs[i].rows[row]);
    }
    fprintf(file, "}},\n");
  }
  fprintf(file, "};\n\nconst unsigned terminus_font_glyph_count = %zu;\n", set->count);
  fclose(file);
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: bdf_to_kernel_font INPUT.bdf OUTPUT.c OUTPUT.h\n");
    return 2;
  }

  struct glyph_set set = {0};
  int width = 0;
  int height = 0;
  parse_bdf(argv[1], &width, &height, &set);
  write_header(argv[3], width, height);
  write_source(argv[2], basename_of(argv[3]), &set, height);
  return 0;
}
