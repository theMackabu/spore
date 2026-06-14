#include "framebuffer.h"

#include "terminus_font.h"

#include <stddef.h>
#include <stdint.h>

enum {
  CSI_PARAM_MAX = 16,
  MAX_TERM_COLS = 256,
  MAX_TERM_ROWS = 128,
  ANSI_DEFAULT_FG = 0xd8dee9,
  ANSI_DEFAULT_BG = 0x000000,
};

struct terminal_cell {
  uint32_t codepoint;
  uint32_t fg;
  uint32_t bg;
  bool bold;
};

static uint8_t *fb_base;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_stride;
static uint32_t fb_format;
static uint32_t term_cols;
static uint32_t term_rows;
static uint32_t cursor_x;
static uint32_t cursor_y;
static uint32_t saved_x;
static uint32_t saved_y;
static uint32_t scroll_top;
static uint32_t scroll_bottom;
static uint32_t alt_saved_x;
static uint32_t alt_saved_y;
static uint32_t alt_saved_scroll_top;
static uint32_t alt_saved_scroll_bottom;
static uint32_t fg_color = ANSI_DEFAULT_FG;
static uint32_t bg_color = ANSI_DEFAULT_BG;
static bool bold;
static bool reverse_video;
static bool insert_mode;
static bool autowrap = true;
static bool wrap_pending;
static uint32_t alt_saved_fg;
static uint32_t alt_saved_bg;
static bool alt_saved_bold;
static bool alt_saved_reverse;
static bool alt_saved_wrap_pending;
static bool cursor_visible = true;
static bool cursor_drawn;
static bool alternate_screen;
static void (*flush_display)(void);
static uint32_t pending_flush_chars;
static uint32_t blink_ticks;
static uint8_t esc_state;
static bool csi_private;
static int csi_params[CSI_PARAM_MAX];
static uint8_t csi_count;
static uint32_t utf8_codepoint;
static uint32_t last_printable = ' ';
static uint8_t utf8_left;
static struct terminal_cell cells[MAX_TERM_COLS * MAX_TERM_ROWS];
static struct terminal_cell primary_cells[MAX_TERM_COLS * MAX_TERM_ROWS];

static void put_codepoint(uint32_t codepoint);
static void redraw_rows(uint32_t top, uint32_t bottom);

static const uint32_t ansi16[16] = {
  0x1d2229, 0xbf616a, 0xa3be8c, 0xebcb8b, 0x81a1c1, 0xb48ead, 0x88c0d0, 0xe5e9f0,
  0x4c566a, 0xd08770, 0xb1d196, 0xf0d98f, 0x8fbcbb, 0xc895bf, 0x8fcedf, 0xffffff,
};

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t color256(unsigned value) {
  if (value < 16) { return ansi16[value]; }
  if (value < 232) {
    unsigned cube = value - 16;
    unsigned r = cube / 36;
    unsigned g = (cube / 6) % 6;
    unsigned b = cube % 6;
    static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
    return rgb(levels[r], levels[g], levels[b]);
  }
  if (value < 256) {
    uint8_t gray = (uint8_t)(8 + (value - 232) * 10);
    return rgb(gray, gray, gray);
  }
  return ANSI_DEFAULT_FG;
}

static uint32_t current_fg(void) {
  return reverse_video ? bg_color : fg_color;
}

static uint32_t current_bg(void) {
  return reverse_video ? fg_color : bg_color;
}

static struct terminal_cell *cell_at(uint32_t x, uint32_t y) {
  return &cells[y * MAX_TERM_COLS + x];
}

static void set_blank_cell(uint32_t x, uint32_t y) {
  struct terminal_cell *cell = cell_at(x, y);
  cell->codepoint = ' ';
  cell->fg = current_fg();
  cell->bg = current_bg();
  cell->bold = false;
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= fb_width || y >= fb_height) { return; }
  uint8_t *p = fb_base + ((uint64_t)y * fb_stride + x) * 4u;
  uint8_t r = (uint8_t)(color >> 16);
  uint8_t g = (uint8_t)(color >> 8);
  uint8_t b = (uint8_t)color;
  if (fb_format == SPORE_FB_FORMAT_RGBX8888) {
    p[0] = r;
    p[1] = g;
    p[2] = b;
  } else {
    p[0] = b;
    p[1] = g;
    p[2] = r;
  }
  p[3] = 0;
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  for (uint32_t row = 0; row < h; ++row) {
    for (uint32_t col = 0; col < w; ++col) {
      put_pixel(x + col, y + row, color);
    }
  }
}

static void copy_cell_rows_up(void) {
  uint32_t row_pixels = TERMINUS_FONT_HEIGHT;
  uint32_t bytes_per_line = fb_stride * 4u;
  uint32_t scroll_bytes = row_pixels * bytes_per_line;
  uint32_t visible_bytes = term_rows * row_pixels * bytes_per_line;
  uint8_t *dst = fb_base;
  uint8_t *src = fb_base + scroll_bytes;
  for (uint32_t i = 0; i < visible_bytes - scroll_bytes; ++i) {
    dst[i] = src[i];
  }
  for (uint32_t row = 1; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      *cell_at(col, row - 1u) = *cell_at(col, row);
    }
  }
  for (uint32_t col = 0; col < term_cols; ++col) {
    set_blank_cell(col, term_rows - 1u);
  }
  fill_rect(0, (term_rows - 1u) * row_pixels, term_cols * TERMINUS_FONT_WIDTH, row_pixels, current_bg());
}

static const struct terminus_glyph *find_glyph(uint32_t codepoint) {
  unsigned lo = 0;
  unsigned hi = terminus_font_glyph_count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2u;
    uint32_t value = terminus_font_glyphs[mid].codepoint;
    if (value == codepoint) { return &terminus_font_glyphs[mid]; }
    if (value < codepoint) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  return NULL;
}

static void draw_glyph_at(uint32_t cell_x, uint32_t cell_y, uint32_t codepoint, uint32_t fg, uint32_t bg,
                          bool draw_bold) {
  const struct terminus_glyph *glyph = find_glyph(codepoint);
  if (glyph == NULL) { glyph = find_glyph(0xfffd); }
  if (glyph == NULL) { return; }

  uint32_t px = cell_x * TERMINUS_FONT_WIDTH;
  uint32_t py = cell_y * TERMINUS_FONT_HEIGHT;
  for (uint32_t row = 0; row < TERMINUS_FONT_HEIGHT; ++row) {
    uint16_t bits = glyph->rows[row];
    for (uint32_t col = 0; col < TERMINUS_FONT_WIDTH; ++col) {
      bool on = (bits & (uint16_t)(0x8000u >> col)) != 0;
      if (!on && draw_bold && col > 0) { on = (bits & (uint16_t)(0x8000u >> (col - 1u))) != 0; }
      put_pixel(px + col, py + row, on ? fg : bg);
    }
  }
}

static void redraw_cell(uint32_t x, uint32_t y) {
  const struct terminal_cell *cell = cell_at(x, y);
  draw_glyph_at(x, y, cell->codepoint, cell->fg, cell->bg, cell->bold);
}

static void write_cell(uint32_t x, uint32_t y, uint32_t codepoint) {
  struct terminal_cell *cell = cell_at(x, y);
  cell->codepoint = codepoint;
  cell->fg = current_fg();
  cell->bg = current_bg();
  cell->bold = bold;
  redraw_cell(x, y);
}

static void insert_cells(uint32_t n) {
  if (cursor_y >= term_rows || cursor_x >= term_cols) { return; }
  if (n == 0) { n = 1; }
  if (n > term_cols - cursor_x) { n = term_cols - cursor_x; }
  for (uint32_t col = term_cols; col-- > cursor_x + n;) {
    *cell_at(col, cursor_y) = *cell_at(col - n, cursor_y);
  }
  for (uint32_t col = cursor_x; col < cursor_x + n; ++col) {
    set_blank_cell(col, cursor_y);
  }
  redraw_rows(cursor_y, cursor_y);
}

static void delete_cells(uint32_t n) {
  if (cursor_y >= term_rows || cursor_x >= term_cols) { return; }
  if (n == 0) { n = 1; }
  if (n > term_cols - cursor_x) { n = term_cols - cursor_x; }
  for (uint32_t col = cursor_x; col + n < term_cols; ++col) {
    *cell_at(col, cursor_y) = *cell_at(col + n, cursor_y);
  }
  for (uint32_t col = term_cols - n; col < term_cols; ++col) {
    set_blank_cell(col, cursor_y);
  }
  redraw_rows(cursor_y, cursor_y);
}

static void erase_cell(uint32_t x, uint32_t y) {
  set_blank_cell(x, y);
  redraw_cell(x, y);
}

static void erase_display(int mode);
static void reset_scroll_region(void) {
  scroll_top = 0;
  scroll_bottom = term_rows == 0 ? 0 : term_rows - 1u;
}

static void redraw_screen(void) {
  for (uint32_t row = 0; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      redraw_cell(col, row);
    }
  }
}

static void enter_alternate_screen(void) {
  if (alternate_screen) { return; }
  for (uint32_t row = 0; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      primary_cells[row * MAX_TERM_COLS + col] = *cell_at(col, row);
    }
  }
  alt_saved_x = cursor_x;
  alt_saved_y = cursor_y;
  alt_saved_scroll_top = scroll_top;
  alt_saved_scroll_bottom = scroll_bottom;
  alt_saved_fg = fg_color;
  alt_saved_bg = bg_color;
  alt_saved_bold = bold;
  alt_saved_reverse = reverse_video;
  alt_saved_wrap_pending = wrap_pending;
  alternate_screen = true;
  cursor_x = 0;
  cursor_y = 0;
  wrap_pending = false;
  reset_scroll_region();
  erase_display(2);
}

static void leave_alternate_screen(void) {
  if (!alternate_screen) { return; }
  for (uint32_t row = 0; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      *cell_at(col, row) = primary_cells[row * MAX_TERM_COLS + col];
    }
  }
  cursor_x = alt_saved_x;
  cursor_y = alt_saved_y;
  scroll_top = alt_saved_scroll_top;
  scroll_bottom = alt_saved_scroll_bottom;
  fg_color = alt_saved_fg;
  bg_color = alt_saved_bg;
  bold = alt_saved_bold;
  reverse_video = alt_saved_reverse;
  wrap_pending = alt_saved_wrap_pending;
  alternate_screen = false;
  redraw_screen();
}

static void draw_cursor(void) {
  if (!cursor_visible || cursor_drawn || cursor_x >= term_cols || cursor_y >= term_rows) { return; }
  uint32_t px = cursor_x * TERMINUS_FONT_WIDTH;
  uint32_t py = cursor_y * TERMINUS_FONT_HEIGHT + TERMINUS_FONT_HEIGHT - 3u;
  fill_rect(px, py, TERMINUS_FONT_WIDTH, 3, current_fg());
  cursor_drawn = true;
}

static void redraw_rows(uint32_t top, uint32_t bottom) {
  if (bottom >= term_rows) { bottom = term_rows - 1u; }
  for (uint32_t row = top; row <= bottom; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      redraw_cell(col, row);
    }
  }
}

static void scroll_region_up(uint32_t top, uint32_t bottom, uint32_t count) {
  if (top >= term_rows || bottom >= term_rows || top > bottom || count == 0) { return; }
  uint32_t height = bottom - top + 1u;
  if (count > height) { count = height; }

  for (uint32_t row = top; row + count <= bottom; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      *cell_at(col, row) = *cell_at(col, row + count);
    }
  }
  for (uint32_t row = bottom - count + 1u; row <= bottom; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      set_blank_cell(col, row);
    }
  }
  redraw_rows(top, bottom);
}

static void scroll_region_down(uint32_t top, uint32_t bottom, uint32_t count) {
  if (top >= term_rows || bottom >= term_rows || top > bottom || count == 0) { return; }
  uint32_t height = bottom - top + 1u;
  if (count > height) { count = height; }

  for (uint32_t row = bottom + 1u; row-- > top + count;) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      *cell_at(col, row) = *cell_at(col, row - count);
    }
  }
  for (uint32_t row = top; row < top + count; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      set_blank_cell(col, row);
    }
  }
  redraw_rows(top, bottom);
}

static void hide_cursor(void) {
  if (!cursor_drawn) { return; }
  if (cursor_x < term_cols && cursor_y < term_rows) { redraw_cell(cursor_x, cursor_y); }
  cursor_drawn = false;
}

static void scroll_if_needed(void) {
  while (cursor_y > scroll_bottom) {
    scroll_region_up(scroll_top, scroll_bottom, 1);
    cursor_y = scroll_bottom;
  }
  while (cursor_y >= term_rows) {
    scroll_region_up(0, term_rows - 1u, 1);
    cursor_y = term_rows - 1u;
  }
}

static int param_or(uint8_t index, int fallback) {
  if (index >= csi_count || csi_params[index] < 0) { return fallback; }
  return csi_params[index];
}

static void reset_attrs(void) {
  fg_color = ANSI_DEFAULT_FG;
  bg_color = ANSI_DEFAULT_BG;
  bold = false;
  reverse_video = false;
}

static void repeat_last_printable(uint32_t n) {
  if (n == 0) { n = 1; }
  for (uint32_t i = 0; i < n; ++i) {
    put_codepoint(last_printable);
  }
}

static void sgr(void) {
  if (csi_count == 0) {
    reset_attrs();
    return;
  }

  for (uint8_t i = 0; i < csi_count; ++i) {
    int p = param_or(i, 0);
    if (p == 0) {
      reset_attrs();
    } else if (p == 1) {
      bold = true;
    } else if (p == 7) {
      reverse_video = true;
    } else if (p == 22) {
      bold = false;
    } else if (p == 27) {
      reverse_video = false;
    } else if (p == 39) {
      fg_color = ANSI_DEFAULT_FG;
    } else if (p == 49) {
      bg_color = ANSI_DEFAULT_BG;
    } else if (p >= 30 && p <= 37) {
      fg_color = ansi16[p - 30];
    } else if (p >= 90 && p <= 97) {
      fg_color = ansi16[p - 90 + 8];
    } else if (p >= 40 && p <= 47) {
      bg_color = ansi16[p - 40];
    } else if (p >= 100 && p <= 107) {
      bg_color = ansi16[p - 100 + 8];
    } else if ((p == 38 || p == 48) && i + 2 < csi_count && csi_params[i + 1] == 5) {
      uint32_t color = color256((unsigned)param_or(i + 2, 7));
      if (p == 38) {
        fg_color = color;
      } else {
        bg_color = color;
      }
      i += 2;
    } else if ((p == 38 || p == 48) && i + 4 < csi_count && csi_params[i + 1] == 2) {
      uint32_t color = rgb((uint8_t)param_or(i + 2, 0), (uint8_t)param_or(i + 3, 0), (uint8_t)param_or(i + 4, 0));
      if (p == 38) {
        fg_color = color;
      } else {
        bg_color = color;
      }
      i += 4;
    }
  }
}

static void erase_display(int mode) {
  if (mode == 2 || mode == 3) {
    for (uint32_t row = 0; row < term_rows; ++row) {
      for (uint32_t col = 0; col < term_cols; ++col) {
        erase_cell(col, row);
      }
    }
    return;
  }
  if (mode == 1) {
    for (uint32_t row = 0; row < cursor_y; ++row) {
      for (uint32_t col = 0; col < term_cols; ++col) {
        erase_cell(col, row);
      }
    }
    for (uint32_t col = 0; col <= cursor_x && col < term_cols; ++col) {
      erase_cell(col, cursor_y);
    }
    return;
  }
  for (uint32_t col = cursor_x; col < term_cols; ++col) {
    erase_cell(col, cursor_y);
  }
  for (uint32_t row = cursor_y + 1u; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      erase_cell(col, row);
    }
  }
}

static void erase_line(int mode) {
  if (mode == 1) {
    for (uint32_t col = 0; col <= cursor_x && col < term_cols; ++col) {
      erase_cell(col, cursor_y);
    }
  } else if (mode == 2) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      erase_cell(col, cursor_y);
    }
  } else {
    for (uint32_t col = cursor_x; col < term_cols; ++col) {
      erase_cell(col, cursor_y);
    }
  }
}

static void erase_chars(uint32_t n) {
  if (n == 0) { n = 1; }
  if (cursor_x + n > term_cols) { n = term_cols - cursor_x; }
  for (uint32_t col = 0; col < n; ++col) {
    erase_cell(cursor_x + col, cursor_y);
  }
}

static void set_scroll_region(void) {
  int top = param_or(0, 1);
  int bottom = param_or(1, (int)term_rows);
  if (top < 1) { top = 1; }
  if (bottom < 1) { bottom = (int)term_rows; }
  if ((uint32_t)bottom > term_rows) { bottom = (int)term_rows; }
  if (top >= bottom) {
    reset_scroll_region();
    cursor_x = 0;
    cursor_y = 0;
    wrap_pending = false;
    return;
  }
  scroll_top = (uint32_t)(top - 1);
  scroll_bottom = (uint32_t)(bottom - 1);
  cursor_x = 0;
  cursor_y = 0;
  wrap_pending = false;
}

static void csi_finish(char final) {
  if (final != 'm' && final != 'b') { wrap_pending = false; }
  switch (final) {
  case 'm':
    sgr();
    break;
  case 'H':
  case 'f': {
    int row = param_or(0, 1);
    int col = param_or(1, 1);
    cursor_y = row <= 1 ? 0 : (uint32_t)(row - 1);
    cursor_x = col <= 1 ? 0 : (uint32_t)(col - 1);
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    if (cursor_x >= term_cols) { cursor_x = term_cols - 1u; }
    break;
  }
  case 'A': {
    uint32_t n = (uint32_t)param_or(0, 1);
    cursor_y = n > cursor_y ? 0 : cursor_y - n;
    break;
  }
  case 'B':
    cursor_y += (uint32_t)param_or(0, 1);
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    break;
  case 'C':
    cursor_x += (uint32_t)param_or(0, 1);
    if (cursor_x >= term_cols) { cursor_x = term_cols - 1u; }
    break;
  case 'a':
    cursor_x += (uint32_t)param_or(0, 1);
    if (cursor_x >= term_cols) { cursor_x = term_cols - 1u; }
    break;
  case 'D': {
    uint32_t n = (uint32_t)param_or(0, 1);
    cursor_x = n > cursor_x ? 0 : cursor_x - n;
    break;
  }
  case 'E':
    cursor_y += (uint32_t)param_or(0, 1);
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    cursor_x = 0;
    break;
  case 'F': {
    uint32_t n = (uint32_t)param_or(0, 1);
    cursor_y = n > cursor_y ? 0 : cursor_y - n;
    cursor_x = 0;
    break;
  }
  case 'G':
  case '`':
    cursor_x = param_or(0, 1) <= 1 ? 0 : (uint32_t)(param_or(0, 1) - 1);
    if (cursor_x >= term_cols) { cursor_x = term_cols - 1u; }
    break;
  case 'd':
    cursor_y = param_or(0, 1) <= 1 ? 0 : (uint32_t)(param_or(0, 1) - 1);
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    break;
  case 'e':
    cursor_y += (uint32_t)param_or(0, 1);
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    break;
  case 'J':
    erase_display(param_or(0, 0));
    break;
  case 'K':
    erase_line(param_or(0, 0));
    break;
  case '@':
    insert_cells((uint32_t)param_or(0, 1));
    break;
  case 'L': {
    uint32_t bottom = cursor_y > scroll_bottom ? term_rows - 1u : scroll_bottom;
    scroll_region_down(cursor_y, bottom, (uint32_t)param_or(0, 1));
    break;
  }
  case 'P':
    delete_cells((uint32_t)param_or(0, 1));
    break;
  case 'M': {
    uint32_t bottom = cursor_y > scroll_bottom ? term_rows - 1u : scroll_bottom;
    scroll_region_up(cursor_y, bottom, (uint32_t)param_or(0, 1));
    break;
  }
  case 'S':
    scroll_region_up(scroll_top, scroll_bottom, (uint32_t)param_or(0, 1));
    break;
  case 'T':
    scroll_region_down(scroll_top, scroll_bottom, (uint32_t)param_or(0, 1));
    break;
  case 'X':
    erase_chars((uint32_t)param_or(0, 1));
    break;
  case 'b':
    repeat_last_printable((uint32_t)param_or(0, 1));
    break;
  case 'r':
    set_scroll_region();
    break;
  case 's':
    saved_x = cursor_x;
    saved_y = cursor_y;
    break;
  case 'u':
    cursor_x = saved_x;
    cursor_y = saved_y;
    break;
  case 'h':
    if (csi_private && param_or(0, 0) == 25) {
      cursor_visible = true;
    } else if (csi_private && param_or(0, 0) == 7) {
      autowrap = true;
    } else if (csi_private && param_or(0, 0) == 1049) {
      enter_alternate_screen();
    } else if (!csi_private && param_or(0, 0) == 4) {
      insert_mode = true;
    }
    break;
  case 'l':
    if (csi_private && param_or(0, 0) == 25) {
      cursor_visible = false;
      hide_cursor();
    } else if (csi_private && param_or(0, 0) == 7) {
      autowrap = false;
    } else if (csi_private && param_or(0, 0) == 1049) {
      leave_alternate_screen();
    } else if (!csi_private && param_or(0, 0) == 4) {
      insert_mode = false;
    }
    break;
  default:
    break;
  }
}

static void csi_add_digit(char c) {
  if (csi_count == 0) {
    csi_count = 1;
    csi_params[0] = -1;
  }
  if (csi_params[csi_count - 1u] < 0) { csi_params[csi_count - 1u] = 0; }
  csi_params[csi_count - 1u] = csi_params[csi_count - 1u] * 10 + (c - '0');
}

static void csi_next_param(void) {
  if (csi_count == 0) {
    csi_count = 1;
    csi_params[0] = -1;
  }
  if (csi_count < CSI_PARAM_MAX) { csi_params[csi_count++] = -1; }
}

static void put_codepoint(uint32_t codepoint) {
  if (codepoint == '\n') {
    wrap_pending = false;
    cursor_x = 0;
    ++cursor_y;
    scroll_if_needed();
    return;
  }
  if (codepoint == '\r') {
    wrap_pending = false;
    cursor_x = 0;
    return;
  }
  if (codepoint == '\b') {
    wrap_pending = false;
    if (cursor_x > 0) { --cursor_x; }
    return;
  }
  if (codepoint == '\t') {
    wrap_pending = false;
    cursor_x = (cursor_x + 8u) & ~7u;
    if (cursor_x >= term_cols) {
      cursor_x = 0;
      ++cursor_y;
      scroll_if_needed();
    }
    return;
  }
  if (codepoint < 0x20) { return; }

  if (wrap_pending) {
    cursor_x = 0;
    ++cursor_y;
    wrap_pending = false;
    scroll_if_needed();
  }
  last_printable = codepoint;
  if (insert_mode) { insert_cells(1); }
  write_cell(cursor_x, cursor_y, codepoint);
  if (cursor_x + 1u >= term_cols) {
    if (autowrap) {
      wrap_pending = true;
    } else {
      cursor_x = term_cols - 1u;
    }
  } else {
    ++cursor_x;
  }
}

static void feed_printable_byte(uint8_t byte) {
  if (utf8_left == 0) {
    if (byte < 0x80) {
      put_codepoint(byte);
    } else if ((byte & 0xe0u) == 0xc0u) {
      utf8_codepoint = byte & 0x1fu;
      utf8_left = 1;
    } else if ((byte & 0xf0u) == 0xe0u) {
      utf8_codepoint = byte & 0x0fu;
      utf8_left = 2;
    } else if ((byte & 0xf8u) == 0xf0u) {
      utf8_codepoint = byte & 0x07u;
      utf8_left = 3;
    } else {
      put_codepoint(0xfffd);
    }
    return;
  }
  if ((byte & 0xc0u) != 0x80u) {
    utf8_left = 0;
    put_codepoint(0xfffd);
    feed_printable_byte(byte);
    return;
  }
  utf8_codepoint = (utf8_codepoint << 6) | (byte & 0x3fu);
  --utf8_left;
  if (utf8_left == 0) { put_codepoint(utf8_codepoint); }
}

static void terminal_feed(uint8_t byte) {
  if (esc_state == 2) {
    if (byte == 0x07 || byte == 0x18 || byte == 0x1a) { esc_state = 0; }
    if (byte == 0x1b) { esc_state = 3; }
    return;
  }
  if (esc_state == 3) {
    esc_state = byte == '\\' ? 0 : 2;
    return;
  }
  if (esc_state == 5) {
    esc_state = 0;
    return;
  }
  if (esc_state == 1) {
    if (byte == '[') {
      esc_state = 4;
      csi_private = false;
      csi_count = 0;
      return;
    }
    if (byte == ']') {
      esc_state = 2;
      return;
    }
    if (byte == '(' || byte == ')' || byte == '*' || byte == '+' || byte == '-' || byte == '.' || byte == '/' ||
        byte == '%') {
      esc_state = 5;
      return;
    }
    if (byte == '7') {
      saved_x = cursor_x;
      saved_y = cursor_y;
    } else if (byte == '8') {
      cursor_x = saved_x;
      cursor_y = saved_y;
      wrap_pending = false;
    } else if (byte == 'c') {
      cursor_x = 0;
      cursor_y = 0;
      saved_x = 0;
      saved_y = 0;
      reset_scroll_region();
      insert_mode = false;
      autowrap = true;
      wrap_pending = false;
      reset_attrs();
      erase_display(2);
    } else if (byte == 'D') {
      wrap_pending = false;
      ++cursor_y;
      scroll_if_needed();
    } else if (byte == 'E') {
      wrap_pending = false;
      cursor_x = 0;
      ++cursor_y;
      scroll_if_needed();
    } else if (byte == 'M') {
      wrap_pending = false;
      if (cursor_y == scroll_top) {
        scroll_region_down(scroll_top, scroll_bottom, 1);
      } else if (cursor_y > 0) {
        --cursor_y;
      }
    }
    esc_state = 0;
    return;
  }
  if (esc_state == 4) {
    if (byte == '?') {
      csi_private = true;
    } else if (byte >= '0' && byte <= '9') {
      csi_add_digit((char)byte);
    } else if (byte == ';' || byte == ':') {
      csi_next_param();
    } else if (byte >= 0x40 && byte <= 0x7e) {
      csi_finish((char)byte);
      esc_state = 0;
    }
    return;
  }
  if (byte == 0x1b) {
    esc_state = 1;
    utf8_left = 0;
    return;
  }
  feed_printable_byte(byte);
}

static bool configure_framebuffer(const struct spore_boot_info *boot, bool preserve) {
  if (boot == NULL || boot->framebuffer_phys == 0 || boot->framebuffer_size == 0 || boot->framebuffer_width == 0 ||
      boot->framebuffer_height == 0 || boot->framebuffer_pixels_per_scanline < boot->framebuffer_width ||
      (boot->framebuffer_format != SPORE_FB_FORMAT_RGBX8888 && boot->framebuffer_format != SPORE_FB_FORMAT_BGRX8888)) {
    return false;
  }

  uint32_t new_cols = boot->framebuffer_width / TERMINUS_FONT_WIDTH;
  uint32_t new_rows = boot->framebuffer_height / TERMINUS_FONT_HEIGHT;
  if (new_cols < 40 || new_rows < 12 || new_cols > MAX_TERM_COLS || new_rows > MAX_TERM_ROWS) { return false; }

  uint32_t old_cols = term_cols;
  uint32_t old_rows = term_rows;
  fb_base = (uint8_t *)(uintptr_t)(boot->hhdm_offset + boot->framebuffer_phys);
  fb_width = boot->framebuffer_width;
  fb_height = boot->framebuffer_height;
  fb_stride = boot->framebuffer_pixels_per_scanline;
  fb_format = boot->framebuffer_format;
  term_cols = new_cols;
  term_rows = new_rows;

  cursor_drawn = false;
  pending_flush_chars = 0;
  blink_ticks = 0;

  if (!preserve) {
    cursor_x = 0;
    cursor_y = 0;
    saved_x = 0;
    saved_y = 0;
    cursor_visible = true;
    alternate_screen = false;
    reset_scroll_region();
    insert_mode = false;
    autowrap = true;
    wrap_pending = false;
    esc_state = 0;
    utf8_left = 0;
    reset_attrs();
  } else {
    if (cursor_x >= term_cols) { cursor_x = term_cols - 1u; }
    if (cursor_y >= term_rows) { cursor_y = term_rows - 1u; }
    if (saved_x >= term_cols) { saved_x = term_cols - 1u; }
    if (saved_y >= term_rows) { saved_y = term_rows - 1u; }
    if (alt_saved_x >= term_cols) { alt_saved_x = term_cols - 1u; }
    if (alt_saved_y >= term_rows) { alt_saved_y = term_rows - 1u; }
    if (scroll_top >= term_rows || scroll_bottom >= term_rows || scroll_top >= scroll_bottom) { reset_scroll_region(); }
    if (alt_saved_scroll_top >= term_rows || alt_saved_scroll_bottom >= term_rows ||
        alt_saved_scroll_top >= alt_saved_scroll_bottom) {
      alt_saved_scroll_top = 0;
      alt_saved_scroll_bottom = term_rows - 1u;
    }
  }

  for (uint32_t row = 0; row < term_rows; ++row) {
    for (uint32_t col = 0; col < term_cols; ++col) {
      bool new_cell = !preserve || row >= old_rows || col >= old_cols;
      if (new_cell) {
        set_blank_cell(col, row);
        primary_cells[row * MAX_TERM_COLS + col] = *cell_at(col, row);
      }
    }
  }
  fill_rect(0, 0, term_cols * TERMINUS_FONT_WIDTH, term_rows * TERMINUS_FONT_HEIGHT, bg_color);
  redraw_screen();
  draw_cursor();
  return true;
}

bool framebuffer_init(const struct spore_boot_info *boot) {
  return configure_framebuffer(boot, false);
}

bool framebuffer_resize(const struct spore_boot_info *boot) {
  return configure_framebuffer(boot, fb_base != NULL);
}

bool framebuffer_ready(void) {
  return fb_base != NULL;
}

void framebuffer_get_winsize(uint16_t *rows, uint16_t *cols) {
  *rows = fb_base == NULL ? 0 : (uint16_t)term_rows;
  *cols = fb_base == NULL ? 0 : (uint16_t)term_cols;
}

void framebuffer_set_flush(void (*flush)(void)) {
  flush_display = flush;
}

void framebuffer_flush(void) {
  if (flush_display == NULL) { return; }
  pending_flush_chars = 0;
  flush_display();
}

void framebuffer_tick(void) {
  if (fb_base == NULL || flush_display == NULL) { return; }
  if (pending_flush_chars != 0) { framebuffer_flush(); }
  if (!cursor_visible) { return; }
  ++blink_ticks;
  if (blink_ticks < 200) { return; }
  blink_ticks = 0;
  if (cursor_drawn) {
    hide_cursor();
  } else {
    draw_cursor();
  }
  framebuffer_flush();
}

void framebuffer_putc(char c) {
  if (fb_base == NULL) { return; }
  hide_cursor();
  terminal_feed((uint8_t)c);
  draw_cursor();
  blink_ticks = 0;
  ++pending_flush_chars;
  if (pending_flush_chars >= 128) { framebuffer_flush(); }
}
