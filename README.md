# 🚀 AscentOS

**64-bit Açık Kaynak Hobi İşletim Sistemi**  
Dosya sistemi desteğiyle geliştirilen eğlenceli bir hobi proje.

## 📋 Özellikler

- 64-bit x86 mimari
- Kendi dosya sistemi implementasyonu
- Metin modu çalıştırma desteği
- Basit Grafiksel Arayüz (GUI) modu
- QEMU emülatörü ile hızlı test

## 🛠️ Nasıl Derlenir ve Çalıştırılır?

### Gereksinimler
- Git
- Make
- QEMU
- x86_64 çapraz derleyici (genellikle repo ile uyumlu toolchain)

### Adımlar

1. Repoyu klonlayın:
   ```bash
   git clone https://github.com/Offihito/AscentOS
2. Klasöre girin:
 ```bash
 cd AscentOS
Çalıştırma seçenekleri:

Metin modu için:
make run

GUI modu için:
make run-gui

Ek komutlar:
Sadece derlemek için: make
Temizlik için: make clean
 
