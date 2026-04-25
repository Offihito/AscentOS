#!/bin/sh
# Xfbdev ile X server başlat ve xeyes'i çalıştır

echo "X server başlatılıyor..."

# X server'ı arka planda başlat
Xfbdev :0 -retro -xkbdir /share/X11/xkb \
    -mouse evdev,,device=/dev/input/event1 \
    -keybd evdev,,device=/dev/input/event0 &

# X server'ın tam olarak başlaması için kısa bir bekleme
sleep 3

echo "X server hazır, xeyes başlatılıyor..."

# DISPLAY ortam değişkenini ayarla ve xeyes'i çalıştır
export DISPLAY=:0

# xeyes root klasöründe ise tam yolu kullan
if [ -x /xeyes ]; then
    /xeyes &
else
    echo "Hata: /xeyes bulunamadı veya çalıştırılabilir değil!"
    exit 1
fi

echo "xeyes başlatıldı. Gözler fareyi takip etmeli."