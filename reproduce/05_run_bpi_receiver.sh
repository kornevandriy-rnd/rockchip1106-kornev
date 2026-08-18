#!/bin/sh
# 05_run_bpi_receiver.sh — приймач НА BPI-M7 (RK3588): апаратний H.265-декод + HDMI.
# Ставить статичний IP на Ethernet-порт BPI (щоб дістатись плати) і запускає
# gstreamer-клієнт до sender'а на RV1106.
#
# Запускати САМЕ НА BPI (armsom@armsom-sige5), не на ноуті/платі.
# Env:
#   BOARD_IP  IP плати RV1106            (деф. 192.168.50.2)
#   PORT      TCP-порт sender'а          (деф. 5000)
#   IFACE     Ethernet-порт BPI          (деф. end0)
#   BPI_IP    статична адреса BPI/CIDR   (деф. 192.168.50.10/24)
#   DISPLAY   X-дисплей для виводу        (деф. :0)
set -e
: "${BOARD_IP:=192.168.50.2}"
: "${PORT:=5000}"
: "${IFACE:=end0}"
: "${BPI_IP:=192.168.50.10/24}"
: "${DISPLAY:=:0}"
export DISPLAY

echo "== IP $BPI_IP на $IFACE (щоб дістати плату $BOARD_IP) =="
sudo ip addr add "$BPI_IP" dev "$IFACE" 2>/dev/null || true   # 'File exists' — не страшно
sudo ip link set "$IFACE" up

echo "== перевірка зв'язку з платою =="
if ! sudo ping -c2 -W2 "$BOARD_IP" >/dev/null 2>&1; then
    echo "!!! Плата $BOARD_IP не відповідає."
    echo "    Перевір: кабель у $IFACE; на платі камера через ЖИВЛЕНИЙ хаб + СПІЛЬНА ЗЕМЛЯ"
    echo "    (без землі плата ребутиться від камери — див. BPI_M7_GROUND.md)."
    exit 1
fi
echo "плата на місці."

echo "== приймач (HW-декод mppvideodec -> HDMI). Ctrl-C = стоп =="
echo "   Спершу на платі має бути запущений sender (див. BPI_M7_GROUND.md, Термінал 1)."
# Низька затримка: БЕЗ videoconvert (він жере CPU і давав джиттер/дропи на 60fps),
# NV12 з декодера йде прямо в апаратний Rockchip-сінк; leaky-черга тримає лише
# найсвіжіший кадр. Якщо rkximagesink недоступний — заміни на glimagesink sync=false.
SINK="${SINK:-rkximagesink sync=false}"
exec gst-launch-1.0 tcpclientsrc host="$BOARD_IP" port="$PORT" \
    ! h265parse ! mppvideodec \
    ! queue leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 \
    ! $SINK
