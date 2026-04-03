#include "console/console.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/pic.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "io/io.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "shell/shell.h"
#include <limine.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((used,
               section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_paging_mode_request
    paging_mode_request = {.id = LIMINE_PAGING_MODE_REQUEST_ID,
                           .revision = 0,
                           .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
                           .max_mode = 0,
                           .min_mode = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

static void halt(void) {
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static void print_uint64(uint64_t num) {
  if (num == 0) {
    console_puts("0");
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    i--;
    char str[2] = {buf[i], '\0'};
    console_puts(str);
  }
}

void kmain(void) {
  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    halt();
  }

  if (framebuffer_request.response == NULL ||
      framebuffer_request.response->framebuffer_count < 1) {
    halt();
  }

  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  console_init(fb);

  if (paging_mode_request.response != NULL) {
    if (paging_mode_request.response->mode == LIMINE_PAGING_MODE_X86_64_4LVL) {
      console_puts("[OK] Limine Paging Mode: 4-level (x86_64)\n");
    } else if (paging_mode_request.response->mode ==
               LIMINE_PAGING_MODE_X86_64_5LVL) {
      console_puts("[OK] Limine Paging Mode: 5-level (x86_64)\n");
    } else {
      console_puts("[OK] Limine Paging Mode: Unknown\n");
    }
  } else {
    console_puts("[WARN] Paging mode response not provided by Limine.\n");
  }

  gdt_init();
  idt_init();

  // Set up Programmable Interrupt Controller
  pic_remap(32, 40);
  outb(0x21, 0xFC); // 0xFC = 11111100 (IRQ0 inside PIT and IRQ1 inside Keyboard unmasked)
  outb(0xA1, 0xFF); // Everything masked on Slave
  
  pit_init(100);    // Start PIT at 100 Hz
  console_puts("[OK] Legacy PIC Remapped and PIT 100Hz started.\n");

  keyboard_init();
  __asm__ volatile("sti"); // Enable hardware interrupts!

  if (memmap_request.response == NULL || hhdm_request.response == NULL) {
    console_puts(
        "[ERR] Missing Limine memory map or HHDM responses. Halting.\n");
    halt();
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);

  console_puts("[OK] Physical Memory Manager (PMM) Initialized.\n");
  console_puts("     Total RAM:  ");
  print_uint64(pmm_get_total_memory() / (1024 * 1024));
  console_puts(" MB\n");

  console_puts("     Usable RAM: ");
  print_uint64(pmm_get_usable_memory() / (1024 * 1024));
  console_puts(" MB\n\n");

  console_puts("[OK] Reclaiming Limine Bootloader Memory...\n");
  pmm_reclaim_bootloader();
  console_puts("     Optimized Usable RAM: ");
  print_uint64(pmm_get_usable_memory() / (1024 * 1024));
  console_puts(" MB\n\n");

  console_puts("[OK] Initializing Virtual Memory Manager (VMM)...\n");
  vmm_init();
  console_puts("     Active CR3 Page Map hooked.\n");

  console_puts("[OK] Testing VMM mapping... (0xCAFEBABE000)\n");
  void *test_phys = pmm_alloc();
  if (test_phys) {
    uint64_t vaddr = 0xCAFEBABE000;
    vmm_map_page(vmm_get_active_pml4(), vaddr, (uint64_t)test_phys,
                 PAGE_FLAG_RW | PAGE_FLAG_USER);

    volatile uint64_t *test_ptr = (volatile uint64_t *)vaddr;
    *test_ptr = 0x1337BEEF; // If this page faults, the mapping failed!

    if (*test_ptr == 0x1337BEEF) {
      console_puts("     VMM custom mapping test SUCCESSFUL!\n");
    } else {
      console_puts("     VMM custom mapping test FAILED!\n");
    }
  }

  console_puts("\nKernel initialization complete. Starting shell...\n");

  console_puts("[OK] Initializing Kernel Heap...\n");
  heap_init();
  char *heap_test = kmalloc(64);
  if (heap_test) {
    const char *test_msg = "     Heap allocation SUCCESSFUL!\n";
    // avoid string.h for speed, just copy manually or include it
    int i = 0;
    while (test_msg[i] != '\0') {
      heap_test[i] = test_msg[i];
      i++;
    }
    heap_test[i] = '\0';
    console_puts(heap_test);
    kfree(heap_test);
  } else {
    console_puts("     Heap allocation FAILED!\n");
  }

  shell_init();
  shell_run();
}
