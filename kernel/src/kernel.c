#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "console/console.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

static void halt(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        halt();
    }

    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count < 1) {
        halt();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    console_init(fb);
    console_puts("Hello World from AscentOS!\n");
    
    gdt_init();
    idt_init();
    
    // console_puts("Testing Hardware Page Fault (INT 14)...\n");
    // console_puts("Attempting to write to unmapped address 0xDEADBEEF...\n\n");
    
    // volatile uint64_t *bad_ptr = (volatile uint64_t *)0xDEADBEEF;
    // *bad_ptr = 42; 

    console_puts("Kernel initialization complete. Halting.\n");
    halt();
}
