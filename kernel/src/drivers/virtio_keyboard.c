#include "virtio_keyboard.h"

#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "pl011.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  VIRTIO_MMIO_BASE = 0x0a000000,
  VIRTIO_MMIO_SLOT_SIZE = 0x200,
  VIRTIO_MMIO_SLOTS = 32,
  VIRTIO_MMIO_MAGIC = 0x000,
  VIRTIO_MMIO_VERSION = 0x004,
  VIRTIO_MMIO_DEVICE_ID = 0x008,
  VIRTIO_MMIO_DEVICE_FEATURES = 0x010,
  VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
  VIRTIO_MMIO_DRIVER_FEATURES = 0x020,
  VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
  VIRTIO_MMIO_QUEUE_SEL = 0x030,
  VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034,
  VIRTIO_MMIO_QUEUE_NUM = 0x038,
  VIRTIO_MMIO_QUEUE_READY = 0x044,
  VIRTIO_MMIO_QUEUE_NOTIFY = 0x050,
  VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
  VIRTIO_MMIO_INTERRUPT_ACK = 0x064,
  VIRTIO_MMIO_STATUS = 0x070,
  VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080,
  VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
  VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
  VIRTIO_MAGIC = 0x74726976,
  VIRTIO_DEVICE_INPUT = 18,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
  EVENT_QUEUE = 0,
  QUEUE_SIZE = 64,
  VIRTQ_DESC_F_WRITE = 2,
  EV_KEY = 1,
  KEY_BACKSPACE = 14,
  KEY_ENTER = 28,
  KEY_LEFTCTRL = 29,
  KEY_LEFTSHIFT = 42,
  KEY_RIGHTSHIFT = 54,
  KEY_CAPSLOCK = 58,
  KEY_RIGHTCTRL = 97,
  KEY_UP = 103,
  KEY_LEFT = 105,
  KEY_RIGHT = 106,
  KEY_DOWN = 108,
};

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[QUEUE_SIZE];
  uint16_t used_event;
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[QUEUE_SIZE];
  uint16_t avail_event;
};

struct virtio_input_event {
  uint16_t type;
  uint16_t code;
  uint32_t value;
};

static uint64_t hhdm;
static uint64_t mmio_base;
static bool keyboard_ready;
static bool shift_down;
static bool ctrl_down;
static bool caps_lock;
static uint64_t desc_pa;
static uint64_t avail_pa;
static uint64_t used_pa;
static uint64_t events_pa;
static struct virtq_desc *desc;
static struct virtq_avail *avail;
static volatile struct virtq_used *used;
static struct virtio_input_event *events;
static uint16_t avail_idx;
static uint16_t used_idx;

static volatile uint32_t *reg32(uint64_t offset) {
  return (volatile uint32_t *)(uintptr_t)(hhdm + mmio_base + offset);
}

static uint32_t read32(uint64_t offset) {
  return *reg32(offset);
}

static void write32(uint64_t offset, uint32_t value) {
  *reg32(offset) = value;
  __asm__ volatile("dsb sy" : : : "memory");
}

static void set_status(uint32_t bits) {
  write32(VIRTIO_MMIO_STATUS, read32(VIRTIO_MMIO_STATUS) | bits);
}

static uint64_t alloc_zero_page(void **virt) {
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) {
    *virt = NULL;
    return 0;
  }
  *virt = (void *)(uintptr_t)(hhdm + pa);
  kmemset(*virt, 0, 4096);
  return pa;
}

static void write_pa_pair(uint64_t low_reg, uint64_t pa) {
  write32(low_reg, (uint32_t)pa);
  write32(low_reg + 4, (uint32_t)(pa >> 32));
}

static void enqueue_event_desc(uint16_t id) {
  avail->ring[avail_idx % QUEUE_SIZE] = id;
  __asm__ volatile("dsb sy" : : : "memory");
  avail->idx = (uint16_t)(avail_idx + 1u);
  avail_idx = avail->idx;
}

static void emit_str(const char *s) {
  while (*s != '\0') {
    pl011_inject_input(*s++);
  }
}

static char key_ascii(uint16_t code) {
  static const char normal[] = {
    [2] = '1',  [3] = '2',  [4] = '3',   [5] = '4',  [6] = '5',   [7] = '6',  [8] = '7',  [9] = '8',
    [10] = '9', [11] = '0', [12] = '-',  [13] = '=', [16] = 'q',  [17] = 'w', [18] = 'e', [19] = 'r',
    [20] = 't', [21] = 'y', [22] = 'u',  [23] = 'i', [24] = 'o',  [25] = 'p', [26] = '[', [27] = ']',
    [30] = 'a', [31] = 's', [32] = 'd',  [33] = 'f', [34] = 'g',  [35] = 'h', [36] = 'j', [37] = 'k',
    [38] = 'l', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z', [45] = 'x', [46] = 'c',
    [47] = 'v', [48] = 'b', [49] = 'n',  [50] = 'm', [51] = ',',  [52] = '.', [53] = '/', [57] = ' ',
  };
  static const char shifted[] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',  [7] = '^',  [8] = '&',
    [9] = '*',  [10] = '(', [11] = ')', [12] = '_', [13] = '+', [26] = '{', [27] = '}',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|', [51] = '<', [52] = '>', [53] = '?',
  };
  if (code >= sizeof(normal)) { return '\0'; }
  char c = normal[code];
  if (c >= 'a' && c <= 'z') {
    if (shift_down != caps_lock) { c = (char)(c - 'a' + 'A'); }
    if (ctrl_down) { c = (char)(c - 'a' + 1); }
    return c;
  }
  if (shift_down && code < sizeof(shifted) && shifted[code] != '\0') { return shifted[code]; }
  return c;
}

static void handle_key(uint16_t code, uint32_t value) {
  bool down = value != 0;
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
    shift_down = down;
    return;
  }
  if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
    ctrl_down = down;
    return;
  }
  if (code == KEY_CAPSLOCK && value == 1) {
    caps_lock = !caps_lock;
    return;
  }
  if (!down) { return; }

  if (code == KEY_ENTER) {
    pl011_inject_input('\r');
  } else if (code == KEY_BACKSPACE) {
    pl011_inject_input(0x7f);
  } else if (code == KEY_UP) {
    emit_str("\033[A");
  } else if (code == KEY_DOWN) {
    emit_str("\033[B");
  } else if (code == KEY_RIGHT) {
    emit_str("\033[C");
  } else if (code == KEY_LEFT) {
    emit_str("\033[D");
  } else {
    char c = key_ascii(code);
    if (c != '\0') { pl011_inject_input(c); }
  }
}

static bool setup_device(uint64_t base) {
  mmio_base = base;
  write32(VIRTIO_MMIO_STATUS, 0);
  set_status(VIRTIO_STATUS_ACKNOWLEDGE);
  set_status(VIRTIO_STATUS_DRIVER);

  write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  uint32_t features_hi = read32(VIRTIO_MMIO_DEVICE_FEATURES);
  if ((features_hi & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) { return false; }
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 1u << VIRTIO_F_VERSION_1_BIT);
  set_status(VIRTIO_STATUS_FEATURES_OK);
  if ((read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) { return false; }

  write32(VIRTIO_MMIO_QUEUE_SEL, EVENT_QUEUE);
  if (read32(VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE) { return false; }
  write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

  desc_pa = alloc_zero_page((void **)&desc);
  avail_pa = alloc_zero_page((void **)&avail);
  used_pa = alloc_zero_page((void **)&used);
  events_pa = alloc_zero_page((void **)&events);
  if (desc_pa == 0 || avail_pa == 0 || used_pa == 0 || events_pa == 0) { return false; }

  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    desc[i].addr = events_pa + (uint64_t)i * sizeof(struct virtio_input_event);
    desc[i].len = sizeof(struct virtio_input_event);
    desc[i].flags = VIRTQ_DESC_F_WRITE;
    desc[i].next = 0;
    enqueue_event_desc(i);
  }

  write_pa_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_pa);
  write32(VIRTIO_MMIO_QUEUE_READY, 1);
  set_status(VIRTIO_STATUS_DRIVER_OK);
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, EVENT_QUEUE);

  used_idx = used->idx;
  keyboard_ready = true;
  kprintf("[spore] virtio-keyboard: mmio %p up\n", (void *)(uintptr_t)mmio_base);
  return true;
}

bool virtio_keyboard_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  keyboard_ready = false;
  shift_down = false;
  ctrl_down = false;
  caps_lock = false;

  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_SIZE;
    mmio_base = base;
    if (read32(VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC && read32(VIRTIO_MMIO_VERSION) == 2 &&
        read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEVICE_INPUT) {
      return setup_device(base);
    }
  }
  return false;
}

bool virtio_keyboard_poll(void) {
  if (!keyboard_ready) { return false; }
  bool input = false;
  __asm__ volatile("dsb sy" : : : "memory");
  while (used->idx != used_idx) {
    uint16_t ring = used_idx % QUEUE_SIZE;
    uint32_t id = used->ring[ring].id;
    if (id < QUEUE_SIZE) {
      struct virtio_input_event ev = events[id];
      if (ev.type == EV_KEY) {
        handle_key(ev.code, ev.value);
        input = true;
      }
      enqueue_event_desc((uint16_t)id);
    }
    ++used_idx;
  }
  if (input) {
    uint32_t isr = read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr != 0) { write32(VIRTIO_MMIO_INTERRUPT_ACK, isr); }
    write32(VIRTIO_MMIO_QUEUE_NOTIFY, EVENT_QUEUE);
  }
  return input;
}
