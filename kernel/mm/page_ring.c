#include "mm/page_ring.h"
#include "mm/pmm.h"

static void free_slot(struct page_ring *ring, uint32_t slot) {
  pmm_free_page(ring->pages[slot]);
  ring->pages[slot] = 0;
  --ring->allocated;
}

void page_ring_clear(struct page_ring *ring) {
  for (uint32_t i = 0; i < ring->capacity / PAGE_SIZE; ++i) {
    if (ring->pages[i]) { free_slot(ring, i); }
  }
  ring->head = ring->used = 0;
}

static uint32_t span(uint32_t pos, uint32_t remaining) {
  uint32_t room = PAGE_SIZE - pos % PAGE_SIZE;
  return remaining < room ? remaining : room;
}

void page_ring_consume(struct page_ring *ring, uint32_t bytes) {
  uint32_t start = ring->head, end = start + bytes;
  ring->used -= bytes;
  if (!ring->used) {
    uint32_t spare = ((end - 1) % ring->capacity) / PAGE_SIZE;
    uint64_t pa = ring->pages[spare];
    ring->pages[spare] = 0;
    if (ring->allocated > 1) { page_ring_clear(ring); }
    ring->pages[0] = pa;
    ring->head = 0;
    return;
  }
  ring->head = end % ring->capacity;
  uint32_t tail = ((end + ring->used - 1) % ring->capacity) / PAGE_SIZE;
  for (uint32_t page = start / PAGE_SIZE; page < end / PAGE_SIZE; ++page) {
    uint32_t slot = page % (ring->capacity / PAGE_SIZE);
    if (slot != tail) { free_slot(ring, slot); }
  }
}

int page_ring_write(struct page_ring *ring, uint32_t len, ring_copy_in copy, void *source) {
  if (len > ring->capacity - ring->used) { len = ring->capacity - ring->used; }
  uint32_t pos = (ring->head + ring->used) % ring->capacity;
  uint64_t added = 0;
  int error = 0;
  for (uint32_t done = 0; done < len;) {
    uint32_t slot = pos / PAGE_SIZE;
    if (!ring->pages[slot]) {
      ring->pages[slot] = pmm_alloc_page();
      if (!ring->pages[slot]) { error = -12; break; }
      ++ring->allocated;
      added |= UINT64_C(1) << slot;
    }
    uint32_t size = span(pos, len - done);
    uint8_t *page = pmm_phys_to_virt(ring->pages[slot]);
    error = copy(source, page + pos % PAGE_SIZE, size);
    if (error) { break; }
    done += size;
    pos = (pos + size) % ring->capacity;
  }
  if (error) {
    while (added) {
      free_slot(ring, (uint32_t)__builtin_ctzll(added));
      added &= added - 1;
    }
    return error;
  }
  ring->used += len;
  return (int)len;
}

int page_ring_read(struct page_ring *ring, uint32_t len, bool peek, ring_copy_out copy, void *destination) {
  if (len > ring->used) { len = ring->used; }
  uint32_t pos = ring->head;
  for (uint32_t done = 0; done < len;) {
    uint32_t size = span(pos, len - done);
    const uint8_t *page = pmm_phys_to_virt(ring->pages[pos / PAGE_SIZE]);
    int error = copy(destination, page + pos % PAGE_SIZE, size);
    if (error) { return error; }
    done += size;
    pos = (pos + size) % ring->capacity;
  }
  if (len && !peek) { page_ring_consume(ring, len); }
  return (int)len;
}
