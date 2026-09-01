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
# Що робить:
#   1) keepalive до Beam RX: поки плата «пінгує» Beam і слухає порт 5700, Beam
#      тримає потік. Без слухача ядро шле Бімові ICMP port-unreachable — і Beam
#      ГЛУШИТЬ відео (саме тому пасивний перегляд «вмирав»);
#   2) приймає RTP, АПАРАТНО декодує (mppvideodec) і виводить на HDMI
#      (rkximagesink). Самовідновний цикл — після будь-якого зриву підіймається сам.
#
# Персистентні передумови (ставляться ОДИН РАЗ, див. bpi/README.md):
#   - end1 має статику 192.168.1.2/24 (nmcli-профіль beam-ground);
#   - /etc/sysctl.d/99-beam.conf:
#         net.core.rmem_max=16777216           (великий буфер під густий потік)
#         net.ipv4.ping_group_range=0 2147483647  (ping без root, для keepalive)
#
# Запуск: systemd-служба thermal-receiver.service (User=armsom, DISPLAY=:0).
# Env (перевизначувані): BEAM_RX (192.168.1.21), VIDEO_PORT (5700), DISPLAY (:0).

BEAM_RX="${BEAM_RX:-192.168.1.21}"   # наземний Beam — кому шлемо keepalive
VIDEO_PORT="${VIDEO_PORT:-5700}"     # порт, на який Beam штовхає відео
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/armsom/.Xauthority}"

# пауза на підняття мережі/екрана після завантаження
sleep 5

# 1) keepalive у фоні: тримає потік живим (Beam бачить приймача, ARP свіжий).
#    ping без root працює завдяки net.ipv4.ping_group_range (див. вище).
( while true; do ping -c 10 "$BEAM_RX" >/dev/null 2>&1; sleep 1; done ) &
KEEPALIVE=$!
trap 'kill "$KEEPALIVE" 2>/dev/null' EXIT INT TERM

# 2) приймач з апаратним декодом, самовідновний.
#    - buffer-size=4M: густий потік дрібних пакетів не переповнює сокет;
#    - rtpjitterbuffer: збирає фрагменти назад у кадри, гасить джитер;
#    - queue leaky=downstream: «клапан» перед синком — не дає пайплайну застигнути
#      (без нього був «кадр і стоп»).
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
