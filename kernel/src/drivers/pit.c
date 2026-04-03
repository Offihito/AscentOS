#include "drivers/pit.h"
#include "cpu/isr.h"
#include "io/io.h"
#include "console/console.h"
#include <stdbool.h>

#define PIT_CMD_PORT   0x43
#define PIT_DATA_PORT0 0x40

static uint64_t pit_ticks = 0;
static bool cursor_state = false;

static void pit_callback(struct registers *regs) {
    (void)regs;
    pit_ticks++;

    // Toggle cursor state every 50 ticks (usually 500ms if freq is 100Hz)
    if (pit_ticks % 50 == 0) {
        cursor_state = !cursor_state;
        console_update_cursor(cursor_state);
    }
}

void pit_init(uint32_t frequency) {
    // Register our callback for IRQ0 (Interrupt 32)
    register_interrupt_handler(32, pit_callback);

    // Hardware clock is 1193180 Hz
    uint32_t divisor = 1193180 / frequency;

    // Send the command byte
    // 0x36: Channel 0, Access LSB then MSB, Mode 3 (Square Wave Generator), 16-bit binary
    outb(PIT_CMD_PORT, 0x36);

    // Send the frequency divisor
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)((divisor >> 8) & 0xFF);

    outb(PIT_DATA_PORT0, l);
    outb(PIT_DATA_PORT0, h);
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}
