#ifndef CPU_H
#define CPU_H

// CPU bilgi yapısı
typedef struct {
    unsigned int usage_percent;
    char vendor[13];
    char brand[49];  // "AMD Ryzen 5 7600 6-Core Processor" gibi
    unsigned int family;
    unsigned int model;
    unsigned int stepping;
} CPUInfo;

// CPU fonksiyonları
void init_cpu();
void update_cpu_usage();
unsigned int get_cpu_usage();
void get_cpu_info(CPUInfo* info);
const char* get_cpu_vendor();
const char* get_cpu_brand();

#endif