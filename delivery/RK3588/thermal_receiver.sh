#!/bin/bash
# thermal_receiver.sh — наземний приймач Beam на BPI-M7 (RK3588).  РОБОЧИЙ ВАРІАНТ.
#
#   Beam RX (192.168.1.21) --RTP/UDP H.265--> 192.168.1.2:5700 --> ffmpeg --> gst --> HDMI
#
# Чому саме так (перевірено на місці):
#   - Beam стрімить, ЛИШЕ поки приймач (а) шле RTCP і (б) БЕЗПЕРЕРВНО пінгує Beam.
#     Голий `gst udpsrc` RTCP не шле → сам не заводить. `ffmpeg` шле — тому приймає він.
#   - Радіо дає втрати → апаратний mppvideodec на «битому» H.265 чорніє;
#     тому декодуємо СОФТОВО (ffmpeg), а gst лише показує.
#   - Синк: rkximagesink малює тільки апаратні кадри → для софт-кадрів чорний.
#     Робочі для звичайних кадрів: xvimagesink / glimagesink (див. SINK нижче).
#
# Env (перевизначувані):
#   BEAM_RX=192.168.1.21  GCS_IP=192.168.1.2  VIDEO_PORT=5700
#   SINK=xvimagesink      (постав той, що реально малює на твоєму образі)
#   FF=~/bin/ffmpeg       (статичний ffmpeg; setup.sh кладе сюди)
#   WIDTH=1280 HEIGHT=1024 FPS=25

BEAM_RX="${BEAM_RX:-192.168.1.21}"
GCS_IP="${GCS_IP:-192.168.1.2}"
VIDEO_PORT="${VIDEO_PORT:-5700}"
SINK="${SINK:-xvimagesink}"
FF="${FF:-$HOME/bin/ffmpeg}"
WIDTH="${WIDTH:-1280}"; HEIGHT="${HEIGHT:-1024}"; FPS="${FPS:-25}"
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/home/armsom/.Xauthority}"

sleep 5   # дати мережі/екрану піднятись після завантаження

# 1) адреса 192.168.1.2 має бути на порту, у якому кабель Beam (end0 АБО end1).
ensure_ip() {
    ip -o addr show 2>/dev/null | grep -q "$GCS_IP" && return 0
    for P in end0 end1; do
        ip -o link show "$P" 2>/dev/null | grep -q LOWER_UP || continue
        [ "$P" = end0 ] && sudo nmcli con up beam-e0 2>/dev/null || sudo nmcli con up beam-e1 2>/dev/null
        return 0
    done
    return 1
}
i=0; while [ $i -lt 30 ]; do ensure_ip && break; i=$((i+1)); sleep 2; done

# 2) SDP-опис RTP-потоку.
SDP=/tmp/beam.sdp
printf 'v=0\no=- 0 0 IN IP4 %s\ns=beam\nc=IN IP4 %s\nt=0 0\nm=video %s RTP/AVP 96\na=rtpmap:96 H265/90000\n' \
    "$GCS_IP" "$GCS_IP" "$VIDEO_PORT" > "$SDP"

# 3) БЕЗПЕРЕРВНИЙ keepalive-пінг (без нього Beam глушить потік).
( while true; do sudo ping -i 1 "$BEAM_RX" >/dev/null 2>&1; sleep 1; done ) &
KEEPALIVE=$!
trap 'kill "$KEEPALIVE" 2>/dev/null; sudo pkill -9 -f "ping -i 1 $BEAM_RX" 2>/dev/null' EXIT INT TERM

# 4) приймач: ffmpeg (RTP+RTCP, софт-декод) -> gst -> HDMI, самовідновно.
while true; do
    "$FF" -protocol_whitelist file,udp,rtp -buffer_size 8388608 -reorder_queue_size 2000 \
        -fflags nobuffer -flags low_delay -i "$SDP" -an -pix_fmt yuv420p -f rawvideo - 2>/dev/null | \
    gst-launch-1.0 fdsrc ! rawvideoparse width="$WIDTH" height="$HEIGHT" format=i420 framerate="$FPS/1" \
        ! videoconvert ! "$SINK" sync=false
    sleep 2
done
