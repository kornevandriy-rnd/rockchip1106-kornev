#!/bin/sh
# upscale-frame.sh — апскейл кадру термокамери під дисплей (СОФТОВИЙ, ffmpeg).
# Запускати НА платі. Вхід — сирий YUYV 640x512 АБО готовий PNG/JPG.
#
#   sh upscale-frame.sh <in.yuyv|in.png> [OUT_PNG] [SIZE]
#   напр.:  sh upscale-frame.sh /root/frame.yuyv /root/up_720.png 720
#
# УВАГА: це ТИМЧАСОВИЙ софтовий шлях (жере CPU). Фінальний, апаратний варіант —
# RGA (YUYV->RGB + масштаб за ~4.6 мс, майже без CPU), але він потребує
# крос-компільованої програми на librga: на самій платі компілятора НЕМАЄ.
# Скейл-ліміт RGA: 0.0625..16x; підтримує вхід YUYV422, вихід RGBA/RGB888/RGB565.

IN="${1:-/root/frame.yuyv}"
OUT="${2:-/root/up_720.png}"
SZ="${3:-720}"
W=640
H=512

if [ ! -s "$IN" ]; then
    echo "ПОМИЛКА: нема вхідного файлу '$IN'" >&2
    exit 1
fi

# Вписуємо кадр 640x512 (5:4) у квадрат SZxSZ із чорними полями (letterbox)
PAD="scale=${SZ}:${SZ}:force_original_aspect_ratio=decrease:flags=lanczos,pad=${SZ}:${SZ}:(ow-iw)/2:(oh-ih)/2:black"

case "$IN" in
    *.yuyv|*.raw|*.YUYV)
        ffmpeg -y -f rawvideo -pix_fmt yuyv422 -s ${W}x${H} -i "$IN" \
            -vf "normalize,${PAD}" "$OUT" >/dev/null 2>&1
        ;;
    *)
        ffmpeg -y -i "$IN" -vf "$PAD" "$OUT" >/dev/null 2>&1
        ;;
esac

if [ -s "$OUT" ]; then
    echo "OK: $OUT (${SZ}x${SZ}, $(wc -c < "$OUT") байт)"
else
    echo "ПОМИЛКА ffmpeg (перевір, чи є ffmpeg і правильний формат входу)" >&2
    exit 1
fi
