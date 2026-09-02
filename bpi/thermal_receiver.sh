#!/bin/bash
# thermal_receiver.sh — наземний приймач тепловізійного відео на BPI-M7 (RK3588).
#
# Джерело: цифровий радіоканал Sine.video Beam.
#   RV1106 (IP-камера, RTSP) --)) Beam TX (((- радіо -))) Beam RX ((-- ЦЯ ПЛАТА.
#
# Наземний Beam (RX, 192.168.1.21) НЕ віддає RTSP — він ШТОВХАЄ відео як
# RTP/UDP H.265 (payload type 96, clock-rate 90000) на 192.168.1.2:5700
# (порт end1 цієї плати). Пакети дрібні (~129 Б) — Beam ріже H.265 на маленькі
# RTP-фрагменти під радіо; депакетизатор їх збирає.
#
# !!! КЛЮЧОВЕ (перевірено сніфером): Beam стрімить, ЛИШЕ поки приймач
# БЕЗПЕРЕРВНО пінгує Beam RX (192.168.1.21). Пінг стих на кілька секунд — Beam
# глушить потік. Тому нижче — надійний безперервний keepalive-пінг з авто-рестартом.
#
# Що робить:
#   1) чекає/піднімає IP 192.168.1.2 на end1 (NM інколи не робить цього при старті);
#   2) тримає БЕЗПЕРЕРВНИЙ keepalive-пінг на Beam RX (без нього нема відео);
#   3) приймає RTP, АПАРАТНО декодує (mppvideodec) → HDMI (rkximagesink),
#      самовідновний цикл.
#
# Персистентні передумови (один раз, див. bpi/README.md):
#   - nmcli-профіль beam-ground: end1 = 192.168.1.2/24;
#   - /etc/sysctl.d/99-beam.conf: net.core.rmem_max=16777216.
#
# Запуск: systemd-служба thermal-receiver.service (User=armsom, DISPLAY=:0,
# passwordless sudo для armsom обов'язковий — скрипт кличе sudo nmcli/ping).

BEAM_RX="${BEAM_RX:-192.168.1.21}"   # наземний Beam — кому шлемо keepalive
GCS_IP="${GCS_IP:-192.168.1.2}"      # адреса приймача (куди Beam штовхає відео)
IFACE="${IFACE:-end1}"               # порт, у який встромлений Beam RX
VIDEO_PORT="${VIDEO_PORT:-5700}"     # порт, на який Beam штовхає відео
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/armsom/.Xauthority}"

# пауза на підняття мережі/екрана після завантаження
sleep 5

# 1) переконатись, що на end1 є 192.168.1.2 (інакше пінг не піде і відео не буде).
i=0
while [ "$i" -lt 30 ]; do
    ip -o addr show "$IFACE" 2>/dev/null | grep -q "$GCS_IP" && break
    sudo nmcli con up beam-ground 2>/dev/null \
        || sudo ip addr add "$GCS_IP/24" dev "$IFACE" 2>/dev/null
    i=$((i + 1)); sleep 2
done

# 2) БЕЗПЕРЕРВНИЙ keepalive-пінг з авто-рестартом (сам стрім живе лише поки він іде).
( while true; do sudo ping -i 1 "$BEAM_RX" >/dev/null 2>&1; sleep 1; done ) &
KEEPALIVE=$!
# чистий вихід при stop служби (щоб не висіла на таймауті й не лишала root-ping)
trap 'kill "$KEEPALIVE" 2>/dev/null; sudo pkill -9 -f "ping -i 1 $BEAM_RX" 2>/dev/null' EXIT INT TERM

# 3) приймач з апаратним декодом, самовідновний.
#    buffer-size=4M: густий потік дрібних пакетів не переповнює сокет;
#    rtpjitterbuffer: збирає фрагменти в кадри, гасить джитер;
#    queue leaky=downstream: «клапан» перед синком — не дає застигнути («кадр і стоп»).
while true; do
    gst-launch-1.0 \
        udpsrc port="$VIDEO_PORT" buffer-size=4194304 \
        caps="application/x-rtp,media=video,encoding-name=H265,payload=96,clock-rate=90000" \
        ! rtpjitterbuffer latency=100 \
        ! rtph265depay ! h265parse ! mppvideodec \
        ! queue leaky=downstream max-size-buffers=3 max-size-time=0 max-size-bytes=0 \
        ! rkximagesink sync=false
    # сюди — коли потік урвався → перепідключитись
    sleep 2
done
