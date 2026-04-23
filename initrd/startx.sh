#!/bin/sh
# Start X server and xeyes

# Start X server in background
Xfbdev :0 -retro -xkbdir /share/X11/xkb -mouse evdev,,device=/dev/input/event1 -keybd evdev,,device=/dev/input/event0

# Wait for X to start