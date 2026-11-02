#!/usr/bin/env bash
# ops/diagnose.sh — дерево рішень «нема відео на наземці».
# Проходить тракт RV → Beam → RK, знаходить ПЕРШУ ланку, що рветься,
# і друкує ПРИЧИНУ + готову команду-фікс. Нічого не змінює (лише читає).
#
#   ops/diagnose.sh          # швидкий вердикт
#   ops/diagnose.sh --full   # ще й повні звіти rv_check/rk_check
set -u
HERE="$(dirname "$0")"; . "$HERE/lib.sh"

hdr "ДІАГНОСТИКА ТРАКТУ"; show_params
verdict() { printf "\n%s►► ПРИЧИНА:%s %s\n%s►► ФІКС:%s   %s\n" "$R" "$N" "$1" "$G" "$N" "$2"; exit 1; }
done_ok() { printf "\n%s►► Усе зелене на цьому рівні.%s %s\n" "$G" "$N" "$1"; exit 0; }

# ── РІВЕНЬ 1: земля доступна? ────────────────────────────────────────────────
pingable "$RK_HOST" || verdict "RK недоступна ($RK_HOST): вимкнена / змінився host key / інший IP (перепрошивка?)." \
  "увімкни RK; якщо IP інший — знайди (nmap -sn 192.168.0.0/24) і задай RK_HOST; host key: ssh-keygen -R $RK_HOST"

# один ssh — зібрати стан землі
G_OUT="$(rk_ssh 'bash -s' <<EOF 2>/dev/null
GCS_IP="$GCS_IP"; BEAM_RX="$BEAM_RX"; VP="$VIDEO_PORT"
cp=""; for p in end0 end1; do [ "\$(cat /sys/class/net/\$p/carrier 2>/dev/null)" = 1 ] && cp=\$p; done
echo "CARRIER=\$cp"
echo "IPPORT=\$(ip -o addr show | awk -v ip="\$GCS_IP" '\$0 ~ ip{print \$2}' | head -1)"
echo "IPCOUNT=\$(ip -o addr show | grep -c "\$GCS_IP/")"
echo "BEAMPING=\$(ping -c1 -W2 \$BEAM_RX >/dev/null 2>&1 && echo ok || echo no)"
echo "KA=\$(pgrep -f "ping .*\$BEAM_RX" >/dev/null && echo yes || echo no)"
echo "RECV=\$(pgrep -f "ffmpeg|gst-launch-1.0" >/dev/null && echo yes || echo no)"
if [ -n "\$cp" ]; then
  echo "PKTS=\$(sudo timeout 5 tcpdump -n -i \$cp udp port \$VP 2>/dev/null | grep -c '> ')"
else echo "PKTS=0"; fi
EOF
)"
[ -z "$G_OUT" ] && verdict "ssh на RK не вдався (пароль/ключ/перепрошивка)." \
  "постав ключі: ops/setup_keys.sh  (або перевір RK_USER/пароль; якщо перепрошили — ops/board-bringup для RK)"
gv(){ printf '%s\n' "$G_OUT" | sed -n "s/^$1=//p"; }

# ── РІВЕНЬ 2: мережа землі ────────────────────────────────────────────────────
CARRIER=$(gv CARRIER)
[ -z "$CARRIER" ] && verdict "на RK жоден порт не має лінку (кабель Beam не встромлений / не той порт)." \
  "встроми кабель Beam RX у end0/end1 BPI"
IPPORT=$(gv IPPORT); IPCOUNT=$(gv IPCOUNT)
[ "${IPCOUNT:-0}" -gt 1 ] 2>/dev/null && verdict "адреса $GCS_IP на КІЛЬКОХ портах → конфлікт (Destination Host Unreachable)." \
  "лиши адресу лише на порту з кабелем: ops/fix_rk_ip.sh"
[ -z "$IPPORT" ] && verdict "адреса $GCS_IP не піднята на жодному порту." "ops/fix_rk_ip.sh"
[ "$IPPORT" != "$CARRIER" ] && verdict "$GCS_IP на '$IPPORT', а кабель у '$CARRIER' → трафік мимо." \
  "перенеси адресу на порт із кабелем: ops/fix_rk_ip.sh"
ok "мережа землі: $GCS_IP на $CARRIER (порт із кабелем)"
[ "$(gv BEAMPING)" = ok ] && ok "Beam RX $BEAM_RX доступний" \
  || verdict "Beam RX $BEAM_RX недоступний (кабель/живлення Beam / не піднятий лінк)." "перевір кабель і живлення модуля Beam RX"

# ── РІВЕНЬ 3: є пакети відео на землі? ────────────────────────────────────────
if [ "$(gv PKTS)" -gt 0 ] 2>/dev/null; then
  ok "відео-пакети доходять до RK ($(gv PKTS)/5с) — наземний прийом ЖИВИЙ"
  [ "$(gv RECV)" = yes ] && done_ok "Якщо на HDMI чорне — винен СИНК/декод: ops/fix_restart.sh rk (перевір синк xvimagesink/glimagesink)." \
    || verdict "пакети є, але приймач (ffmpeg/gst) не запущений." "запусти приймач: ops/fix_restart.sh rk"
fi
# пакетів нема:
[ "$(gv KA)" = yes ] && ok "keepalive-пінг живий" \
  || verdict "немає keepalive-пінгу на $BEAM_RX → Beam глушить потік." "підніми приймач із keepalive: ops/fix_restart.sh rk"

# keepalive є, пакетів нема → дивимось БОРТ
warn "keepalive є, але пакетів нема → перевіряю борт/радіо"
if pingable "$RV_HOST"; then
  URL="rtsp://${RV_HOST}:${RTSP_PORT}${RTSP_PATH}"
  if command -v ffprobe >/dev/null 2>&1 && timeout 8 ffprobe -rtsp_transport tcp -v error "$URL" >/dev/null 2>&1; then
    verdict "борт віддає RTSP, keepalive є, а на землю не доходить → справа в Beam (Cam IP на TX? RTCP? сесія)." \
      "перевір у TX Cam IP=$RV_HOST ($URL); упевнись, що приймач шле RTCP (ffmpeg, не голий udpsrc)"
  else
    verdict "борт доступний, але RTSP мовчить → камера впала / sender не стартував / зайнятий порт." \
      "на борту: ops/fix_restart.sh rv  (перевір камеру: ops/rv_check.sh)"
  fi
else
  verdict "борт $RV_HOST недоступний із цього місця (нормально, якщо він за Beam) — перевір окремо на борту." \
    "під'єднайся до борту й глянь: ops/rv_check.sh (RV_HOST=<ip борту>)"
fi

[ "${1:-}" = --full ] && { "$HERE/rv_check.sh"; "$HERE/rk_check.sh"; }
