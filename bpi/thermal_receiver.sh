#!/bin/bash
# thermal_receiver.sh — автоприймач H.265-стріму на наземній платі BPI-M7 (RK3588).
#
# Приймає TCP-потік з RV1106 (rv1106_sender :5000), апаратно декодує (mppvideodec)
# і виводить на HDMI (rkximagesink). Самовідновний цикл: після будь-якого зриву
# (плата перезавантажилась, блип USB, рестарт sender'а) перепідключається сам.
#
# Запускається автоматично при вході в графічну сесію (~/.config/autostart).
# Env: BOARD (деф. 192.168.50.2), PORT (деф. 5000), DISPLAY (деф. :0).

BOARD="${BOARD:-192.168.50.2}"
PORT="${PORT:-5000}"
export DISPLAY="${DISPLAY:-:0}"

# невелика пауза на підняття мережі/екрана після завантаження
sleep 5

while true; do
    gst-launch-1.0 tcpclientsrc host="$BOARD" port="$PORT" \
        ! h265parse ! mppvideodec \
        ! queue leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 \
        ! rkximagesink sync=false
    # сюди потрапляємо, коли потік урвався → перепідключитись
    sleep 2
done
