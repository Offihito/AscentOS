#include "cpu.h"

static CPUInfo cpu_info;
static int cpu_initialized = 0;
static unsigned int frame_counter = 0;

// String kopyalama helper
static void str_copy(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

// CPUID çağrısı
static inline void cpuid(unsigned int code, unsigned int* a, unsigned int* b, 
                         unsigned int* c, unsigned int* d) {
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(code)
                 : );
}

// Extended CPUID çağrısı
static inline void cpuid_ext(unsigned int code, unsigned int* a, unsigned int* b, 
                              unsigned int* c, unsigned int* d) {
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(code)
                 : );
}

// CPU vendor bilgisi al
void get_cpu_vendor_string(char* vendor) {
    unsigned int eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);
    
    // Vendor string'i oluştur
    *((unsigned int*)(vendor + 0)) = ebx;
    *((unsigned int*)(vendor + 4)) = edx;
    *((unsigned int*)(vendor + 8)) = ecx;
    vendor[12] = '\0';
}

// CPU brand string al (AMD Ryzen 5 7600 gibi)
void get_cpu_brand_string(char* brand) {
    unsigned int eax, ebx, ecx, edx;
    
    // CPUID 0x80000000 - Extended function support kontrolü
    cpuid_ext(0x80000000, &eax, &ebx, &ecx, &edx);
    
    if (eax < 0x80000004) {
        // Brand string desteklenmiyor
        str_copy(brand, "Unknown CPU", 48);
        return;
    }
    
    // CPUID 0x80000002-0x80000004: Brand string (48 byte)
    char* ptr = brand;
    
    cpuid_ext(0x80000002, &eax, &ebx, &ecx, &edx);
    *((unsigned int*)(ptr + 0)) = eax;
    *((unsigned int*)(ptr + 4)) = ebx;
    *((unsigned int*)(ptr + 8)) = ecx;
    *((unsigned int*)(ptr + 12)) = edx;
    
    cpuid_ext(0x80000003, &eax, &ebx, &ecx, &edx);
    *((unsigned int*)(ptr + 16)) = eax;
    *((unsigned int*)(ptr + 20)) = ebx;
    *((unsigned int*)(ptr + 24)) = ecx;
    *((unsigned int*)(ptr + 28)) = edx;
    
    cpuid_ext(0x80000004, &eax, &ebx, &ecx, &edx);
    *((unsigned int*)(ptr + 32)) = eax;
    *((unsigned int*)(ptr + 36)) = ebx;
    *((unsigned int*)(ptr + 40)) = ecx;
    *((unsigned int*)(ptr + 44)) = edx;
    
    brand[48] = '\0';
    
    // Başındaki boşlukları temizle
    char* start = brand;
    while (*start == ' ') start++;
    
    if (start != brand) {
        int i = 0;
        while (start[i]) {
            brand[i] = start[i];
            i++;
        }
        brand[i] = '\0';
    }
}

// CPU model bilgileri al
void get_cpu_model_info(unsigned int* family, unsigned int* model, unsigned int* stepping) {
    unsigned int eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    
    *stepping = eax & 0xF;
    *model = (eax >> 4) & 0xF;
    *family = (eax >> 8) & 0xF;
    
    // Extended model ve family
    if (*family == 0xF) {
        *family += (eax >> 20) & 0xFF;
    }
    if (*family == 0x6 || *family == 0xF) {
        *model += ((eax >> 16) & 0xF) << 4;
    }
}

// CPU'yu başlat
void init_cpu() {
    if (cpu_initialized) return;
    
    get_cpu_vendor_string(cpu_info.vendor);
    get_cpu_brand_string(cpu_info.brand);
    get_cpu_model_info(&cpu_info.family, &cpu_info.model, &cpu_info.stepping);
    
    cpu_info.usage_percent = 50;
    frame_counter = 0;
    
    cpu_initialized = 1;
}

// CPU kullanımını güncelle (basitleştirilmiş - 64-bit işlem yok)
void update_cpu_usage() {
    if (!cpu_initialized) init_cpu();
    
    frame_counter++;
    
    // Çok düşük, gerçekçi idle kullanımı
    unsigned int base = 5;
    unsigned int variance = ((frame_counter * 7) + (frame_counter >> 2)) % 10;
    
    cpu_info.usage_percent = base + variance; // %5-15 arası
    
    if (frame_counter > 100000) {
        frame_counter = 0;
    }
}

// CPU kullanım yüzdesini al
unsigned int get_cpu_usage() {
    if (!cpu_initialized) {
        init_cpu();
        return 50; // İlk çağrıda varsayılan değer
    }
    return cpu_info.usage_percent;
}

// CPU bilgilerini al
void get_cpu_info(CPUInfo* info) {
    if (!cpu_initialized) init_cpu();
    
    info->family = cpu_info.family;
    info->model = cpu_info.model;
    info->stepping = cpu_info.stepping;
    info->usage_percent = cpu_info.usage_percent;
    
    // Vendor string'i kopyala
    for (int i = 0; i < 13; i++) {
        info->vendor[i] = cpu_info.vendor[i];
    }
    
    // Brand string'i kopyala
    for (int i = 0; i < 49; i++) {
        info->brand[i] = cpu_info.brand[i];
    }
}

// CPU vendor string'ini al
const char* get_cpu_vendor() {
    if (!cpu_initialized) init_cpu();
    return cpu_info.vendor;
}

// CPU brand string'ini al
const char* get_cpu_brand() {
    if (!cpu_initialized) init_cpu();
    return cpu_info.brand;
}