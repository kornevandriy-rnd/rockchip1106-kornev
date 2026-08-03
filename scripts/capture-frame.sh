#!/bin/sh
# capture-frame.sh — захопити ОДИН кадр із USB-UVC термокамери (Seek 640)
# на Luckfox Pico Ultra. Запускати НА платі (root).
#
# Передумови (див. README): USB у host-режимі (luckfox-config) + камера через
# ЖИВЛЕНИЙ USB-хаб (порт плати не витягує струм камери напряму).
#
#   ssh root@192.168.50.2 'sh -s' < scripts/capture-frame.sh
#   або на платі:  sh capture-frame.sh [OUT_PNG] [RETRIES]
#
# Чому так складно:
#  * номер /dev/videoN "плаває" після кожної реенумерації камери — тому шукаємо
#    ноду динамічно за форматом YUYV, а не хардкодимо video21;
#  * камера періодично відвалюється (EMI на лінії плата<->хаб) — тому кілька спроб.

OUT="${1:-/root/frame.png}"
RETRIES="${2:-6}"
W=640
H=512
RAW="/tmp/frame_$$.yuyv"

find_cam() {
    for v in /dev/video*; do
        [ -e "$v" ] || continue
        if v4l2-ctl -d "$v" --list-formats 2>/dev/null | grep -q YUYV; then
            echo "$v"
            return 0
        fi
    done
    return 1
}

rm -f "$RAW"
i=1
while [ "$i" -le "$RETRIES" ]; do
    CAM=$(find_cam)
    if [ -n "$CAM" ]; then
        echo "[$i] нода камери: $CAM"
        v4l2-ctl -d "$CAM" --set-fmt-video=width=$W,height=$H,pixelformat=YUYV \
            --stream-mmap --stream-count=1 --stream-to="$RAW" >/dev/null 2>&1
        if [ -s "$RAW" ]; then
            echo "[$i] кадр захоплено ($(wc -c < "$RAW") байт)"
            break
        fi
        echo "[$i] стрім зірвався (EMI?), повтор..."
    else
        echo "[$i] камери нема на шині (lsusb | grep 3474:43e2), чекаю..."
    fi
    i=$((i + 1))
    sleep 2
done

if [ ! -s "$RAW" ]; then
    echo "ПОМИЛКА: не вдалося захопити кадр за $RETRIES спроб." >&2
    echo "Перевір: extcon USB-HOST=1, живлений хаб, lsusb | grep 3474:43e2" >&2
    exit 1
fi

# YUYV -> PNG з автоконтрастом (normalize) — тепловий кадр часто малоконтрастний
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -y -f rawvideo -pix_fmt yuyv422 -s ${W}x${H} -i "$RAW" -vf normalize "$OUT" >/dev/null 2>&1
    echo "PNG: $OUT ($(wc -c < "$OUT" 2>/dev/null || echo 0) байт)"
else
    cp "$RAW" "${OUT%.png}.yuyv"
    echo "ffmpeg нема; сирий кадр: ${OUT%.png}.yuyv (${W}x${H} YUYV422)"
fi
rm -f "$RAW"
