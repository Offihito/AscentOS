#include "irq.h"
#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "../console/klog.h"
#include "../mm/heap.h"
#include <stddef.h>
#include "lib/string.h"

// Linked list node for shared IRQ handlers
typedef struct irq_handler_node {
    isr_t handler;
    uint64_t hits;
    struct irq_handler_node* next;
} irq_handler_node_t;

// Pool for early-boot allocations before heap is ready
#define EARLY_POOL_SIZE 32
static irq_handler_node_t early_node_pool[EARLY_POOL_SIZE];
static uint32_t early_pool_idx = 0;

// Registry tracking all handlers and flags per legacy IRQ
static struct {
    irq_handler_node_t* handlers;
    uint16_t flags;
    bool registered;
} irq_registry[224];

// Master dispatcher called by the IDT for all managed IRQs
static void irq_master_dispatcher(struct registers *regs) {
    uint8_t irq_no = regs->int_no - 32;
    if (irq_no >= 224) return;

    irq_handler_node_t* node = irq_registry[irq_no].handlers;
    while (node) {
        if (node->handler) {
            node->handler(regs);
            node->hits++; // Track that this handler was called
        }
        node = node->next;
    }
}

static irq_handler_node_t* alloc_node(void) {
    if (early_pool_idx < EARLY_POOL_SIZE) {
        return &early_node_pool[early_pool_idx++];
    }
    return (irq_handler_node_t*)kmalloc(sizeof(irq_handler_node_t));
}

bool irq_install_handler(uint8_t irq_no, isr_t handler, uint16_t flags) {
  if (irq_no >= 224) return false;

  irq_handler_node_t* new_node = alloc_node();
  if (!new_node) return false;
  new_node->handler = handler;
  new_node->hits = 0;
  new_node->next = NULL;

  if (irq_registry[irq_no].handlers == NULL) {
      irq_registry[irq_no].handlers = new_node;
      register_interrupt_handler(32 + irq_no, irq_master_dispatcher);
  } else {
      irq_handler_node_t* curr = irq_registry[irq_no].handlers;
      while (curr->next) curr = curr->next;
      curr->next = new_node;
  }
  irq_registry[irq_no].flags = flags;
  irq_registry[irq_no].registered = true;

  if (ioapic_is_ready() && lapic_is_ready()) {
    uint32_t gsi = irq_no;
    uint16_t route_flags = flags;
    if (route_flags == 0) acpi_get_irq_override(irq_no, &gsi, &route_flags);
    ioapic_route_irq((uint8_t)gsi, 32 + irq_no, (uint8_t)lapic_get_id(), route_flags);
  }
  return true;
}

void irq_manager_sync(void) {
    for (uint8_t i = 0; i < 224; i++) {
        if (irq_registry[i].registered) {
            uint32_t gsi = i;
            uint16_t flags = irq_registry[i].flags;
            if (flags == 0) acpi_get_irq_override(i, &gsi, &flags);
            if (ioapic_is_ready() && lapic_is_ready()) {
                ioapic_route_irq((uint8_t)gsi, 32 + i, (uint8_t)lapic_get_id(), flags);
            }
        }
    }
}

void irq_test_trigger(uint8_t irq_no) {
  uint8_t vector = 32 + irq_no;
  if (vector == 47) { __asm__ volatile("int $47"); }
  else if (vector == 33) { __asm__ volatile("int $33"); }
}
