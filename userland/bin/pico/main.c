#include "pico.h"

#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void handle_key(struct pico_editor *ed, int key) {
  ed->status[0] = '\0';
  if (key != PICO_KEY_CTRL_X) { ed->exit_confirm = false; }
  switch (key) {
  case PICO_KEY_CTRL_G:
    pico_show_help(ed);
    break;
  case PICO_KEY_CTRL_O:
    (void)pico_save_file(ed);
    ed->exit_confirm = false;
    break;
  case PICO_KEY_CTRL_X:
    if (ed->dirty) {
      if (ed->exit_confirm) {
        ed->running = false;
      } else {
        ed->exit_confirm = true;
        pico_set_status(ed, "Unsaved changes: Ctrl-O saves, Ctrl-X again exits");
      }
    } else {
      ed->running = false;
    }
    break;
  case PICO_KEY_CTRL_K:
    pico_cut_line(ed);
    break;
  case PICO_KEY_CTRL_U:
    pico_uncut_line(ed);
    break;
  case PICO_KEY_UP:
    if (ed->cursor_row > 0) { --ed->cursor_row; }
    break;
  case PICO_KEY_DOWN:
    pico_move_down(ed);
    break;
  case PICO_KEY_LEFT:
    if (ed->cursor_col > 0) {
      --ed->cursor_col;
    } else if (ed->cursor_row > 0) {
      --ed->cursor_row;
      ed->cursor_col = pico_line_len(ed, ed->cursor_row);
    }
    break;
  case PICO_KEY_RIGHT:
    if (ed->cursor_col < pico_line_len(ed, ed->cursor_row)) {
      ++ed->cursor_col;
    } else if (ed->cursor_row + 1 < ed->line_count) {
      ++ed->cursor_row;
      ed->cursor_col = 0;
    }
    break;
  case PICO_KEY_HOME:
    ed->cursor_col = 0;
    break;
  case PICO_KEY_END:
    ed->cursor_col = pico_line_len(ed, ed->cursor_row);
    break;
  case PICO_KEY_BACKSPACE:
  case '\b':
    pico_backspace(ed);
    break;
  case PICO_KEY_DELETE:
    pico_delete_char(ed);
    break;
  case '\n':
    pico_insert_newline(ed);
    break;
  default:
    if (key >= 0x20 && key < 0x7f) { pico_insert_char(ed, (char)key); }
    break;
  }
  pico_clamp_cursor(ed);
}

int main(int argc, char **argv) {
  if (argc != 2) { return usage(argv[0], "FILE"); }

  struct pico_editor ed;
  memset(&ed, 0, sizeof(ed));
  ed.running = true;
  pico_init_terminal(&ed);
  if (pico_load_file(&ed, argv[1]) != 0) {
    pico_restore_terminal();
    return EXIT_FAILURE;
  }

  pico_set_status(&ed, "^G Help   ^O Save   ^X Exit");
  while (ed.running) {
    pico_redraw(&ed);
    int key = pico_read_key();
    if (key != PICO_KEY_NONE) { handle_key(&ed, key); }
  }
  pico_restore_terminal();
  return EXIT_SUCCESS;
}
