#!/bin/sh
# check-uvc-readiness.sh — чи готова плата до Кроку 2 (камера Seek 640 по USB-UVC)
# + чи є запасний доступ. Запускати НА платі. POSIX sh / busybox-safe.
#
#   ssh -o HostKeyAlgorithms=+ssh-rsa root@172.32.0.93 'sh -s' < scripts/check-uvc-readiness.sh
#
hr() { echo; echo "===== $* ====="; }

hr "МЕРЕЖА ПЛАТИ (запасний доступ на час камери)"
ip -br link 2>/dev/null || ls /sys/class/net
if [ -e /sys/class/net/eth0 ]; then
  echo ">> eth0 Є — Ethernet доступний (ідеально для ssh/scp під час камери)"
else
  echo ">> eth0 НЕМАЄ — Ethernet без адаптера недоступний; запасний доступ = UART"
fi

hr "UART DEBUG-КОНСОЛЬ"
grep -o 'console=[^ ]*' /proc/cmdline
grep -o 'earlycon=[^ ]*' /proc/cmdline

hr "UVC-HOST ДРАЙВЕР (обовʼязковий, щоб читати камеру по USB)"
find /lib/modules -iname 'uvcvideo*' 2>/dev/null | grep . && echo "(модуль uvcvideo є)" \
  || echo "(модуля uvcvideo НЕ знайдено — можливо вбудований у ядро; див. config нижче)"
if [ -e /proc/config.gz ]; then
  echo "--- kernel config (USB video/gadget/otg) ---"
  zcat /proc/config.gz | grep -iE 'USB_VIDEO_CLASS|USB_CONFIGFS_F_UVC|USB_DWC|USB_CONFIGFS|USB_GADGET=|OTG' || true
else
  echo "(немає /proc/config.gz)"
fi

hr "USB OTG / GADGET (як перемкнути device->host)"
echo "UDC (не порожньо = зараз device/gadget режим):"; ls /sys/class/udc/ 2>/dev/null || echo "(none)"
echo "usb_gadget configfs:"; ls /sys/kernel/config/usb_gadget/ 2>/dev/null || echo "(none)"
for d in /sys/devices/platform/*.usb; do
  [ -e "$d/mode" ] && echo "$(basename "$d")/mode = $(cat "$d/mode" 2>/dev/null)"
done
echo "Luckfox usb-config скрипти:"
find /usr /oem /etc /data 2>/dev/null | grep -iE 'usb_config|usbdevice' | head

hr "RGA / RKNN / MPP / ROCKIT (для upscale)"
find /usr /oem /lib /root 2>/dev/null | grep -iE 'librga|librknn|librockit|librockchip_mpp|rknn' | head -20 \
  || echo "(бібліотек не знайдено)"

hr "V4L2 / MEDIA / ВІДЕО-ІНСТРУМЕНТИ"
for t in v4l2-ctl media-ctl rkaiq_3A_server gst-launch-1.0 ffmpeg rkmedia; do
  p=$(command -v "$t" 2>/dev/null); echo "$t: ${p:-НЕМАЄ}"
done

echo; echo "===== done ====="
