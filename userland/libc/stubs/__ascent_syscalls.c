/*
 * __ascent_syscalls.c
 *
 * musl tüm sistem çağrılarını __syscall() ve __syscall_cp_c() üzerinden yapar.
 * Bu dosya onları AscentOS syscall kapısına yönlendirir.
 *
 * Hiçbir header include edilmez — compiler built-in tipler kullanılır.
 */

long __syscall(long n, long a1, long a2, long a3,
               long a4, long a5, long a6)
{
    long ret;
    __asm__ volatile (
        "movq %1, %%rax\n\t"
        "movq %2, %%rdi\n\t"
        "movq %3, %%rsi\n\t"
        "movq %4, %%rdx\n\t"
        "movq %5, %%r10\n\t"
        "movq %6, %%r8\n\t"
        "movq %7, %%r9\n\t"
        "syscall\n\t"
        "movq %%rax, %0\n\t"
        : "=r"(ret)
        : "r"(n), "r"(a1), "r"(a2), "r"(a3),
          "r"(a4), "r"(a5), "r"(a6)
        : "rax","rdi","rsi","rdx","r10","r8","r9","memory","cc"
    );
    return ret;
}

/* Thread cancellation yok → doğrudan __syscall */
long __syscall_cp_c(long n, long a1, long a2, long a3,
                    long a4, long a5, long a6)
{
    return __syscall(n, a1, a2, a3, a4, a5, a6);
}
