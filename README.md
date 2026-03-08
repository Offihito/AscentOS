# 🚀 AscentOS

> Modern, 64-bit bir hobi işletim sistemi — kendi dosya sistemi ve basit GUI desteğiyle

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
![C](https://img.shields.io/badge/language-C-00599C.svg)
![x86_64](https://img.shields.io/badge/arch-x86__64-important)

## ✨ Mevcut Özellikler (Mart 2026 itibarıyla)

- **64-bit x86_64 mimarisi** — modern donanımlar için tasarlandı
- **FAT32 dosya sistemi** — okuma/yazma desteği çalışıyor
- **Basit metin tabanlı kabuk** (shell)
- **Temel grafik modu** (`gfx` komutu ile geçiş yapılıyor)
- **Modüler bellek yönetimi** (son güncellemelerle stabilize edildi)
- **Musl libc + Newlib** entegrasyonu
- **İlk kullanıcı uygulaması:** [kilo](https://github.com/antirez/kilo) metin editörü port edildi
- **GRUB2 ile önyükleme** (multiboot uyumlu)
- **Temel ağ yığını** hazırlıkları devam ediyor

Şu anda **kendi kendine boot edebiliyor**, FAT32 üzerinden dosya okuyup yazabiliyor ve basit bir GUI moduna geçebiliyor.

## 🛠️ Kaynaktan Derleme & Çalıştırma

### Gereksinimler

- `make`, `gcc` / `clang`, `nasm`, `grub-mkrescue`, `qemu-system-x86_64`
- xorriso (ISO oluşturmak için)
- Musl-cross toolchain (otomatik derleniyor ama istersen manuel kurabilirsin)

### Hızlı Başlangıç

```bash
# Depoyu klonla
git clone https://github.com/Offihito/AscentOS.git
cd AscentOS

# Derle ve QEMU'da çalıştır (metin modu)
make run

# Alternatif: sadece ISO oluştur
make iso