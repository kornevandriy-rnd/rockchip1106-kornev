#!/bin/sh
# board-probe.sh — інвентаризація Luckfox Pico Ultra. Запускати НА платі.
# POSIX sh / busybox-safe (Buildroot).
#
# Запуск із Fedora без копіювання файлу:
#   ssh -o HostKeyAlgorithms=+ssh-rsa root@172.32.0.93 'sh -s' < scripts/board-probe.sh
#
hr() { echo "===== $* ====="; }

hr SYSTEM
uname -a

hr CPU
grep -iE 'processor|model name|Hardware|Revision|Features' /proc/cpuinfo 2>/dev/null || cat /proc/cpuinfo

hr MEMORY
free -h 2>/dev/null || free

hr STORAGE
df -h 2>/dev/null

hr "OS / RELEASE"
cat /etc/os-release 2>/dev/null || cat /etc/issue 2>/dev/null || echo "(немає /etc/os-release)"

hr "KERNEL CMDLINE"
cat /proc/cmdline 2>/dev/null

hr "USB OTG ROLE (host/device)"
found=0
for f in /sys/devices/platform/*.usb/otg/state \
         /sys/devices/platform/*usb*/mode \
         /sys/class/usb_role/*/role; do
  [ -e "$f" ] && { echo "$f = $(cat "$f" 2>/dev/null)"; found=1; }
done
[ "$found" = 0 ] && echo "(індикатор режиму OTG не знайдено в sysfs — див. dmesg)"

hr "VIDEO DEVICES (/dev/video*)"
ls -l /dev/video* 2>/dev/null || echo "немає /dev/video* — камеру ще не підключено або USB у device-режимі (RNDIS)"

hr "MEDIA / V4L-SUBDEV"
ls -l /dev/media* /dev/v4l-subdev* 2>/dev/null || echo "(немає)"

hr "USB DEVICES (sysfs)"
if [ -e /sys/kernel/debug/usb/devices ]; then
  grep -iE 'Manufacturer|Product|SerialNumber|Cls=' /sys/kernel/debug/usb/devices 2>/dev/null
else
  ls /sys/bus/usb/devices/ 2>/dev/null
fi
command -v lsusb >/dev/null 2>&1 && { echo "--- lsusb ---"; lsusb; }

hr "RGA / RKNN бібліотеки (для upscale)"
ls -l /usr/lib/librga* /usr/lib/librknn* /oem/usr/lib/librga* 2>/dev/null || echo "(librga/librknn у стандартних шляхах не знайдено)"

echo "===== done ====="
