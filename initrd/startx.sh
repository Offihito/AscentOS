#!/bin/sh
# Xfbdev ile X server başlat ve twm + xeyes + st çalıştır

echo "X server başlatılıyor..."


rm -f "/tmp/.X0-lock"

# X server'ı arka planda başlat
Xfbdev :0 -retro -xkbdir /share/X11/xkb \
    -mouse evdev,,device=/dev/input/event1 \
    -keybd evdev,,device=/dev/input/event0 &

# X server'ın tam olarak başlaması için bekle
sleep 1

echo "X server hazır, uygulamalar başlatılıyor..."

# DISPLAY ortam değişkenini ayarla
export DISPLAY=:0

# JWM (Pencere yöneticisi önce başlatılmalı)
if [ -x /bin/jwm ]; then
    echo "JWM başlatılıyor..."
    /bin/jwm &
elif command -v jwm >/dev/null 2>&1; then
    echo "JWM (sistem yolu ile) başlatılıyor..."
    jwm &
else
    echo "Uyarı: JWM bulunamadı!"
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

sleep 2


# PATH'i güncelle (Alpine binary'leri için)
export PATH=/usr/bin:/usr/local/bin:$PATH
export LD_LIBRARY_PATH=/usr/lib:/lib:$LD_LIBRARY_PATH

# st (suckless terminal from Alpine)
if [ -x /usr/bin/st ]; then
    echo "st (Alpine) başlatılıyor..."
    /usr/bin/st &
elif command -v st >/dev/null 2>&1; then
    echo "st (sistem yolu ile) başlatılıyor..."
    st &
else
    echo "Uyarı: st (Alpine) bulunamadı!"
fi

echo "Tamamlandı. twm, xeyes ve st çalıştırıldı."