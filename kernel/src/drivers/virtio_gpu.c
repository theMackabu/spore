#include "virtio_gpu.h"

#include "framebuffer.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"

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
  VIRTIO_DEVICE_GPU = 16,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
  CONTROL_QUEUE = 0,
  QUEUE_SIZE = 8,
  VIRTQ_DESC_F_NEXT = 1,
  VIRTQ_DESC_F_WRITE = 2,
  VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
  VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
  VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
  VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
  VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
  VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
  VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
  VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
  VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
  RESOURCE_ID = 1,
  DEFAULT_WIDTH = 1280,
  DEFAULT_HEIGHT = 800,
};

enum gpu_flush_state {
  GPU_FLUSH_IDLE,
  GPU_FLUSH_TRANSFER,
  GPU_FLUSH_RESOURCE_FLUSH,
  GPU_FLUSH_DISPLAY_INFO,
  GPU_FLUSH_CREATE_2D,
  GPU_FLUSH_ATTACH_BACKING,
  GPU_FLUSH_SET_SCANOUT,
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

struct virtio_gpu_ctrl_hdr {
  uint32_t type;
  uint32_t flags;
  uint64_t fence_id;
  uint32_t ctx_id;
  uint32_t padding;
};

struct virtio_gpu_rect {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
};

struct virtio_gpu_display_one {
  struct virtio_gpu_rect rect;
  uint32_t enabled;
  uint32_t flags;
};

struct virtio_gpu_resp_display_info {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_display_one pmodes[16];
};

struct virtio_gpu_resource_create_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t format;
  uint32_t width;
  uint32_t height;
};

struct virtio_gpu_mem_entry {
  uint64_t addr;
  uint32_t length;
  uint32_t padding;
};

struct virtio_gpu_resource_attach_backing {
  struct virtio_gpu_ctrl_hdr hdr;
  uint32_t resource_id;
  uint32_t nr_entries;
  struct virtio_gpu_mem_entry entry;
};

struct virtio_gpu_set_scanout {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect rect;
  uint32_t scanout_id;
  uint32_t resource_id;
};

struct virtio_gpu_transfer_to_host_2d {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect rect;
  uint64_t offset;
  uint32_t resource_id;
  uint32_t padding;
};

struct virtio_gpu_resource_flush {
  struct virtio_gpu_ctrl_hdr hdr;
  struct virtio_gpu_rect rect;
  uint32_t resource_id;
  uint32_t padding;
};

static uint64_t hhdm;
static uint64_t mmio_base;
static bool gpu_ready;
static uint64_t desc_pa;
static uint64_t avail_pa;
static uint64_t used_pa;
static uint64_t req_pa;
static uint64_t resp_pa;
static uint64_t fb_pa;
static uint64_t fb_size;
static struct virtq_desc *desc;
static struct virtq_avail *avail;
static volatile struct virtq_used *used;
static uint8_t *req_buf;
static uint8_t *resp_buf;
static uint16_t avail_idx;
static uint16_t used_idx;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t active_resource_id;
static uint32_t pending_resource_id;
static uint64_t pending_fb_pa;
static uint64_t pending_fb_size;
static uint32_t pending_fb_width;
static uint32_t pending_fb_height;
static uint32_t next_resource_id;
static uint32_t resize_poll_ticks;
static enum gpu_flush_state flush_state;
static bool flush_dirty;

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
  return pa;
}

static uint64_t alloc_zero_contiguous(uint64_t pages, void **virt) {
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (pa == 0) {
    *virt = NULL;
    return 0;
  }
  *virt = (void *)(uintptr_t)(hhdm + pa);
  kmemset(*virt, 0, pages * PAGE_SIZE);
  return pa;
}

static void write_pa_pair(uint64_t low_reg, uint64_t pa) {
  write32(low_reg, (uint32_t)pa);
  write32(low_reg + 4, (uint32_t)(pa >> 32));
}

static bool submit_command(uint32_t req_len, uint32_t resp_len, uint32_t expect_type) {
  kmemset(resp_buf, 0, PAGE_SIZE);
  desc[0].addr = req_pa;
  desc[0].len = req_len;
  desc[0].flags = VIRTQ_DESC_F_NEXT;
  desc[0].next = 1;
  desc[1].addr = resp_pa;
  desc[1].len = resp_len;
  desc[1].flags = VIRTQ_DESC_F_WRITE;
  desc[1].next = 0;
  avail->ring[avail_idx % QUEUE_SIZE] = 0;
  __asm__ volatile("dsb sy" : : : "memory");
  avail->idx = (uint16_t)(avail_idx + 1u);
  avail_idx = avail->idx;
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, CONTROL_QUEUE);

  for (uint32_t spin = 0; spin < 1000000; ++spin) {
    __asm__ volatile("dsb sy" : : : "memory");
    if (used->idx == used_idx) { continue; }
    ++used_idx;
    uint32_t isr = read32(VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr != 0) { write32(VIRTIO_MMIO_INTERRUPT_ACK, isr); }
    const struct virtio_gpu_ctrl_hdr *hdr = (const struct virtio_gpu_ctrl_hdr *)resp_buf;
    return hdr->type == expect_type;
  }
  return false;
}

static bool gpu_cmd_nodata(uint32_t len) {
  return submit_command(len, sizeof(struct virtio_gpu_ctrl_hdr), VIRTIO_GPU_RESP_OK_NODATA);
}

static void submit_async_command(uint32_t req_len, uint32_t resp_len, enum gpu_flush_state next_state) {
  kmemset(resp_buf, 0, PAGE_SIZE);
  desc[0].addr = req_pa;
  desc[0].len = req_len;
  desc[0].flags = VIRTQ_DESC_F_NEXT;
  desc[0].next = 1;
  desc[1].addr = resp_pa;
  desc[1].len = resp_len;
  desc[1].flags = VIRTQ_DESC_F_WRITE;
  desc[1].next = 0;
  avail->ring[avail_idx % QUEUE_SIZE] = 0;
  flush_state = next_state;
  __asm__ volatile("dsb sy" : : : "memory");
  avail->idx = (uint16_t)(avail_idx + 1u);
  avail_idx = avail->idx;
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, CONTROL_QUEUE);
}

static bool read_display_size(uint32_t *width, uint32_t *height) {
  struct virtio_gpu_ctrl_hdr *req = (struct virtio_gpu_ctrl_hdr *)req_buf;
  *req = (struct virtio_gpu_ctrl_hdr){.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO};
  if (!submit_command(sizeof(*req), sizeof(struct virtio_gpu_resp_display_info), VIRTIO_GPU_RESP_OK_DISPLAY_INFO)) {
    return false;
  }

  const struct virtio_gpu_resp_display_info *resp = (const struct virtio_gpu_resp_display_info *)resp_buf;
  for (size_t i = 0; i < 16; ++i) {
    if (resp->pmodes[i].enabled != 0 && resp->pmodes[i].rect.width != 0 && resp->pmodes[i].rect.height != 0) {
      *width = resp->pmodes[i].rect.width;
      *height = resp->pmodes[i].rect.height;
      return true;
    }
  }
  *width = DEFAULT_WIDTH;
  *height = DEFAULT_HEIGHT;
  return true;
}

static bool create_resource(uint32_t resource_id, uint32_t width, uint32_t height) {
  struct virtio_gpu_resource_create_2d *cmd = (struct virtio_gpu_resource_create_2d *)req_buf;
  *cmd = (struct virtio_gpu_resource_create_2d){
    .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D},
    .resource_id = resource_id,
    .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
    .width = width,
    .height = height,
  };
  return gpu_cmd_nodata(sizeof(*cmd));
}

static bool attach_backing(uint32_t resource_id, uint64_t backing_pa, uint64_t backing_size) {
  struct virtio_gpu_resource_attach_backing *cmd = (struct virtio_gpu_resource_attach_backing *)req_buf;
  *cmd = (struct virtio_gpu_resource_attach_backing){
    .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
    .resource_id = resource_id,
    .nr_entries = 1,
    .entry = {.addr = backing_pa, .length = (uint32_t)backing_size},
  };
  return gpu_cmd_nodata(sizeof(*cmd));
}

static bool set_scanout(uint32_t resource_id, uint32_t width, uint32_t height) {
  struct virtio_gpu_set_scanout *cmd = (struct virtio_gpu_set_scanout *)req_buf;
  *cmd = (struct virtio_gpu_set_scanout){
    .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT},
    .rect = {.width = width, .height = height},
    .scanout_id = 0,
    .resource_id = resource_id,
  };
  return gpu_cmd_nodata(sizeof(*cmd));
}

static void submit_display_info(void) {
  struct virtio_gpu_ctrl_hdr *req = (struct virtio_gpu_ctrl_hdr *)req_buf;
  *req = (struct virtio_gpu_ctrl_hdr){.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO};
  submit_async_command(sizeof(*req), sizeof(struct virtio_gpu_resp_display_info), GPU_FLUSH_DISPLAY_INFO);
}

static void submit_create_resource(uint32_t resource_id, uint32_t width, uint32_t height) {
  struct virtio_gpu_resource_create_2d *cmd = (struct virtio_gpu_resource_create_2d *)req_buf;
  *cmd = (struct virtio_gpu_resource_create_2d){
    .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D},
    .resource_id = resource_id,
    .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
    .width = width,
    .height = height,
  };
  submit_async_command(sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr), GPU_FLUSH_CREATE_2D);
}

static void submit_attach_backing(uint32_t resource_id, uint64_t backing_pa, uint64_t backing_size) {
  struct virtio_gpu_resource_attach_backing *cmd = (struct virtio_gpu_resource_attach_backing *)req_buf;
  *cmd = (struct virtio_gpu_resource_attach_backing){
    .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING},
    .resource_id = resource_id,
    .nr_entries = 1,
    .entry = {.addr = backing_pa, .length = (uint32_t)backing_size},
  };
  submit_async_command(sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr), GPU_FLUSH_ATTACH_BACKING);
}

static void submit_set_scanout(uint32_t resource_id, uint32_t width, uint32_t height) {
  struct virtio_gpu_set_scanout *cmd = (struct virtio_gpu_set_scanout *)req_buf;
  *cmd = (struct virtio_gpu_set_scanout){
    .hdr = {.type = VIRTIO_GPU_CMD_SET_SCANOUT},
    .rect = {.width = width, .height = height},
    .scanout_id = 0,
    .resource_id = resource_id,
  };
  submit_async_command(sizeof(*cmd), sizeof(struct virtio_gpu_ctrl_hdr), GPU_FLUSH_SET_SCANOUT);
}

static void submit_transfer(void) {
  struct virtio_gpu_transfer_to_host_2d *transfer = (struct virtio_gpu_transfer_to_host_2d *)req_buf;
  *transfer = (struct virtio_gpu_transfer_to_host_2d){
    .hdr = {.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D},
    .rect = {.width = fb_width, .height = fb_height},
    .offset = 0,
    .resource_id = active_resource_id,
  };
  flush_dirty = false;
  submit_async_command(sizeof(*transfer), sizeof(struct virtio_gpu_ctrl_hdr), GPU_FLUSH_TRANSFER);
}

static void submit_resource_flush(void) {
  struct virtio_gpu_resource_flush *flush = (struct virtio_gpu_resource_flush *)req_buf;
  *flush = (struct virtio_gpu_resource_flush){
    .hdr = {.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH},
    .rect = {.width = fb_width, .height = fb_height},
    .resource_id = active_resource_id,
  };
  submit_async_command(sizeof(*flush), sizeof(struct virtio_gpu_ctrl_hdr), GPU_FLUSH_RESOURCE_FLUSH);
}

static bool display_info_size(uint32_t *width, uint32_t *height) {
  const struct virtio_gpu_resp_display_info *resp = (const struct virtio_gpu_resp_display_info *)resp_buf;
  for (size_t i = 0; i < 16; ++i) {
    if (resp->pmodes[i].enabled != 0 && resp->pmodes[i].rect.width != 0 && resp->pmodes[i].rect.height != 0) {
      *width = resp->pmodes[i].rect.width;
      *height = resp->pmodes[i].rect.height;
      return true;
    }
  }
  return false;
}

static bool prepare_resize(uint32_t width, uint32_t height) {
  uint64_t size = (uint64_t)width * height * 4u;
  if (width == 0 || height == 0 || size > UINT32_MAX) { return false; }
  uint64_t pages = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
  uint8_t *virt = NULL;
  uint64_t pa = alloc_zero_contiguous(pages, (void **)&virt);
  if (pa == 0) { return false; }

  pending_resource_id = next_resource_id++;
  pending_fb_pa = pa;
  pending_fb_size = size;
  pending_fb_width = width;
  pending_fb_height = height;
  return true;
}

static void install_pending_framebuffer(void) {
  fb_pa = pending_fb_pa;
  fb_size = pending_fb_size;
  fb_width = pending_fb_width;
  fb_height = pending_fb_height;
  active_resource_id = pending_resource_id;

  struct spore_boot_info resized = {
    .hhdm_offset = hhdm,
    .framebuffer_phys = fb_pa,
    .framebuffer_size = fb_size,
    .framebuffer_width = fb_width,
    .framebuffer_height = fb_height,
    .framebuffer_pixels_per_scanline = fb_width,
    .framebuffer_format = SPORE_FB_FORMAT_BGRX8888,
  };
  (void)framebuffer_resize(&resized);
  framebuffer_set_flush(virtio_gpu_flush);
  flush_dirty = true;
}

void virtio_gpu_poll(void) {
  if (!gpu_ready) { return; }
  if (flush_state == GPU_FLUSH_IDLE) {
    if (flush_dirty) {
      submit_transfer();
    } else if (++resize_poll_ticks >= 50) {
      resize_poll_ticks = 0;
      submit_display_info();
    }
    return;
  }

  __asm__ volatile("dsb sy" : : : "memory");
  if (used->idx == used_idx) { return; }
  ++used_idx;
  uint32_t isr = read32(VIRTIO_MMIO_INTERRUPT_STATUS);
  if (isr != 0) { write32(VIRTIO_MMIO_INTERRUPT_ACK, isr); }

  struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)resp_buf;
  uint32_t expected =
    flush_state == GPU_FLUSH_DISPLAY_INFO ? VIRTIO_GPU_RESP_OK_DISPLAY_INFO : VIRTIO_GPU_RESP_OK_NODATA;
  if (hdr->type != expected) {
    gpu_ready = false;
    flush_state = GPU_FLUSH_IDLE;
    return;
  }

  enum gpu_flush_state completed = flush_state;
  flush_state = GPU_FLUSH_IDLE;

  if (completed == GPU_FLUSH_TRANSFER) {
    submit_resource_flush();
    return;
  }
  if (completed == GPU_FLUSH_DISPLAY_INFO) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (display_info_size(&width, &height) && (width != fb_width || height != fb_height) &&
        prepare_resize(width, height)) {
      submit_create_resource(pending_resource_id, pending_fb_width, pending_fb_height);
    }
    return;
  }
  if (completed == GPU_FLUSH_CREATE_2D) {
    submit_attach_backing(pending_resource_id, pending_fb_pa, pending_fb_size);
    return;
  }
  if (completed == GPU_FLUSH_ATTACH_BACKING) {
    submit_set_scanout(pending_resource_id, pending_fb_width, pending_fb_height);
    return;
  }
  if (completed == GPU_FLUSH_SET_SCANOUT) {
    install_pending_framebuffer();
    submit_transfer();
    return;
  }

  if (flush_dirty) { submit_transfer(); }
}

void virtio_gpu_flush(void) {
  if (!gpu_ready) { return; }
  flush_dirty = true;
  virtio_gpu_poll();
}

bool virtio_gpu_init(uint64_t hhdm_offset, struct spore_boot_info *fb_boot) {
  hhdm = hhdm_offset;
  gpu_ready = false;
  flush_state = GPU_FLUSH_IDLE;
  flush_dirty = false;
  active_resource_id = RESOURCE_ID;
  next_resource_id = RESOURCE_ID + 1u;
  resize_poll_ticks = 0;
  mmio_base = 0;
  if (fb_boot == NULL) { return false; }

  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_SIZE;
    mmio_base = base;
    if (read32(VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC && read32(VIRTIO_MMIO_VERSION) == 2 &&
        read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEVICE_GPU) {
      break;
    }
    mmio_base = 0;
  }
  if (mmio_base == 0) { return false; }

  write32(VIRTIO_MMIO_STATUS, 0);
  set_status(VIRTIO_STATUS_ACKNOWLEDGE);
  set_status(VIRTIO_STATUS_DRIVER);

  write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  uint32_t features_hi = read32(VIRTIO_MMIO_DEVICE_FEATURES);
  if ((features_hi & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) {
    kprintf("[spore] virtio-gpu: VERSION_1 missing\n");
    return false;
  }
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 1u << VIRTIO_F_VERSION_1_BIT);
  set_status(VIRTIO_STATUS_FEATURES_OK);
  if ((read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
    kprintf("[spore] virtio-gpu: features rejected\n");
    return false;
  }

  write32(VIRTIO_MMIO_QUEUE_SEL, CONTROL_QUEUE);
  if (read32(VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
    kprintf("[spore] virtio-gpu: control queue too small\n");
    return false;
  }
  write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

  desc_pa = alloc_zero_page((void **)&desc);
  avail_pa = alloc_zero_page((void **)&avail);
  used_pa = alloc_zero_page((void **)&used);
  req_pa = alloc_zero_page((void **)&req_buf);
  resp_pa = alloc_zero_page((void **)&resp_buf);
  if (desc_pa == 0 || avail_pa == 0 || used_pa == 0 || req_pa == 0 || resp_pa == 0) {
    kprintf("[spore] virtio-gpu: queue allocation failed\n");
    return false;
  }

  write_pa_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_pa);
  write32(VIRTIO_MMIO_QUEUE_READY, 1);
  avail_idx = 0;
  used_idx = used->idx;
  set_status(VIRTIO_STATUS_DRIVER_OK);

  if (!read_display_size(&fb_width, &fb_height)) {
    kprintf("[spore] virtio-gpu: display info failed\n");
    return false;
  }
  if (fb_width == 0 || fb_height == 0) {
    fb_width = DEFAULT_WIDTH;
    fb_height = DEFAULT_HEIGHT;
  }
  fb_size = (uint64_t)fb_width * fb_height * 4u;
  uint64_t pages = (fb_size + PAGE_SIZE - 1u) / PAGE_SIZE;
  uint8_t *fb_virt = NULL;
  fb_pa = alloc_zero_contiguous(pages, (void **)&fb_virt);
  if (fb_pa == 0 || fb_size > UINT32_MAX) {
    kprintf("[spore] virtio-gpu: framebuffer allocation failed\n");
    return false;
  }

  if (!create_resource(active_resource_id, fb_width, fb_height)) {
    kprintf("[spore] virtio-gpu: resource create failed\n");
    return false;
  }
  if (!attach_backing(active_resource_id, fb_pa, fb_size)) {
    kprintf("[spore] virtio-gpu: attach backing failed\n");
    return false;
  }
  if (!set_scanout(active_resource_id, fb_width, fb_height)) {
    kprintf("[spore] virtio-gpu: set scanout failed\n");
    return false;
  }

  *fb_boot = (struct spore_boot_info){
    .hhdm_offset = hhdm,
    .framebuffer_phys = fb_pa,
    .framebuffer_size = fb_size,
    .framebuffer_width = fb_width,
    .framebuffer_height = fb_height,
    .framebuffer_pixels_per_scanline = fb_width,
    .framebuffer_format = SPORE_FB_FORMAT_BGRX8888,
  };
  gpu_ready = true;
  flush_state = GPU_FLUSH_IDLE;
  flush_dirty = false;
  resize_poll_ticks = 0;
  kprintf("[spore] virtio-gpu: mmio %p %ux%u\n", (void *)(uintptr_t)mmio_base, (unsigned)fb_width, (unsigned)fb_height);
  return true;
}
