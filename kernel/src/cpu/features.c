#include "features.h"
#include <stdint.h>

// Enable SSE/SSE2 for ring 3 (musl/gcc use XMM for string/memory ops). Without
// CR4.OSFXSR, executing those instructions raises #UD (Invalid Opcode).

void cpu_features_init(void) {
  uint64_t cr0;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1ULL << 2); // EM — no x87 emulation
  cr0 |= (1ULL << 1);  // MP — monitor coprocessor (with TS, matches PC behavior)
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1ULL << 9);  // OSFXSR — allow FXSAVE/FXRSTOR + SSE in user mode
  cr4 |= (1ULL << 10); // OSXMMEXCPT — #XF for unmasked SIMD exceptions
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

  __asm__ volatile("fninit");
}
