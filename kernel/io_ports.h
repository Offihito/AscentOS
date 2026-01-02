#ifndef IO_PORTS_H
#define IO_PORTS_H

static inline unsigned char inb(unsigned short port) {
    unsigned char result;
    asm volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(unsigned short port, unsigned char data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline unsigned short inw(unsigned short port) {
    unsigned short result;
    asm volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outw(unsigned short port, unsigned short data) {
    asm volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

static inline unsigned int inl(unsigned short port) {
    unsigned int result;
    asm volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outl(unsigned short port, unsigned int data) {
    asm volatile("outl %0, %1" : : "a"(data), "Nd"(port));
}

#endif