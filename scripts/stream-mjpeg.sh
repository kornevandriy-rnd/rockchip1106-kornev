#!/bin/sh
# stream-mjpeg.sh — ЕКСПЕРИМЕНТАЛЬНО. Живий перегляд камери на ноуті.
# Емітує MJPEG у stdout: цикл КОРОТКИХ одиночних захватів (кожен із ручним
# таймаутом) -> JPEG. Запускати НА платі.
#
# Перегляд на Fedora (пароль не має псувати потік -> SSH ControlMaster):
#   ssh -M -S /tmp/lf.sock -fN root@192.168.50.2                 # пароль ОДИН раз
#   ssh -S /tmp/lf.sock root@192.168.50.2 'sh /tmp/stream-mjpeg.sh' \
#       | ffplay -hide_banner -f image2pipe -framerate 4 -vcodec mjpeg -
#   ssh -S /tmp/lf.sock -O exit root@192.168.50.2                # закрити майстер
#
# ⚠️ ВІДОМЕ ОБМЕЖЕННЯ: на маргінальному USB-лінку (EMI) цикл захватів ЗАКЛИНЮЄ
# камеру (v4l2-ctl застряє в D-state, кадри розсипаються). Потрібен КОРОТКИЙ
# ЕКРАНОВАНИЙ кабель плата<->хаб. Для надійних одиночних кадрів — capture-frame.sh.
#
# Граблі (щоб не вчити заново):
#  * busybox тут БЕЗ `timeout` -> таймаут реалізовано вручну (фон + kill).
#  * ffplay `-f mjpeg` НЕ парсить розмір із живого пайпа -> треба `-f image2pipe`.
#  * запит пароля ssh перебиває статус ffplay -> ОБОВ'ЯЗКОВО ControlMaster (-M -S).
#  * запускати РІВНО ОДНУ копію (кілька конкурують за камеру -> усі виснуть).

W=640
H=512

while true; do
    CAM=""
    for v in /dev/video*; do
        v4l2-ctl -d "$v" --info 2>/dev/null | grep -qi uvc || continue
        v4l2-ctl -d "$v" --list-formats 2>/dev/null | grep -q YUYV || continue
        CAM="$v"; break
    done
    [ -z "$CAM" ] && { sleep 0.5; continue; }
    rm -f /tmp/f.yuyv
    v4l2-ctl -d "$CAM" --set-fmt-video=width=$W,height=$H,pixelformat=YUYV \
        --stream-mmap --stream-count=1 --stream-to=/tmp/f.yuyv >/dev/null 2>&1 &
    p=$!; n=0
    while kill -0 "$p" 2>/dev/null; do
        n=$((n + 1)); [ "$n" -ge 10 ] && { kill -9 "$p" 2>/dev/null; break; }
        sleep 0.5
    done
    wait "$p" 2>/dev/null
    [ -s /tmp/f.yuyv ] && ffmpeg -y -f rawvideo -pix_fmt yuyv422 -s ${W}x${H} \
        -i /tmp/f.yuyv -vf normalize -f mjpeg - 2>/dev/null
    sleep 0.1
done
