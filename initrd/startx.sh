#!/bin/sh
# Xfbdev ile X server başlat ve twm + xeyes + st çalıştır

echo "X server başlatılıyor..."

# X server'ı arka planda başlat
Xfbdev :0 -retro -xkbdir /share/X11/xkb \
    -mouse evdev,,device=/dev/input/event1 \
    -keybd evdev,,device=/dev/input/event0 &

# X server'ın tam olarak başlaması için bekle
sleep 1

echo "X server hazır, uygulamalar başlatılıyor..."

# DISPLAY ortam değişkenini ayarla
export DISPLAY=:0

# twm (Pencere yöneticisi önce başlatılmalı)
if [ -x /twm ]; then
    echo "twm başlatılıyor..."
    /twm &
elif command -v twm >/dev/null 2>&1; then
    echo "twm (sistem yolu ile) başlatılıyor..."
    twm &
else
    echo "Uyarı: twm bulunamadı!"
fi

# xeyes
if [ -x /xeyes ]; then
    echo "xeyes başlatılıyor..."
    /xeyes &
elif command -v xeyes >/dev/null 2>&1; then
    echo "xeyes (sistem yolu ile) başlatılıyor..."
    xeyes &
else
    echo "Uyarı: xeyes bulunamadı!"
fi

# st (suckless terminal)
if [ -x /bin/st ]; then
    echo "st başlatılıyor..."
    /bin/st &
elif command -v st >/dev/null 2>&1; then
    echo "st (sistem yolu ile) başlatılıyor..."
    st &
else
    echo "Uyarı: st bulunamadı!"
fi

echo "Tamamlandı. twm, xeyes ve st çalıştırıldı."