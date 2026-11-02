#!/usr/bin/env bash
# ops/rv_check.sh — стан борту RV1106: мережа, автозапуск, камера, RTSP.
# Використання:  ops/rv_check.sh      (env: RV_HOST, RV_USER, RTSP_PORT, RTSP_PATH)
set -u
. "$(dirname "$0")/lib.sh"

hdr "RV1106 (борт) — ${RV_USER}@${RV_HOST}"

# 1) мережа
if pingable "$RV_HOST"; then ok "ping $RV_HOST"; else
  bad "ping $RV_HOST — не відповідає (плата вимкнена / не той IP / ноут не в тій підмережі)"
  info "перевір: кабель у ноут, адреса ноута в 192.168.1.x, живлення плати"
  exit 2
fi

# 2) ssh + стан автозапуску/камери одним заходом
OUT="$(rv_ssh 'bash -s' <<'EOF' 2>/dev/null
echo "MODE=$(cat /root/thermal_mode 2>/dev/null)"
echo "STATUS<<"
/etc/init.d/S99thermal status 2>/dev/null
echo ">>"
echo "RTSPPID=$(pidof rv1106_rtsp 2>/dev/null)"
echo "TCPPID=$(pidof rv1106_sender 2>/dev/null)"
echo "CAMDEV=$(for d in /sys/bus/usb/devices/*; do [ "$(cat $d/idVendor 2>/dev/null)" = 3474 ] && cat $d/devnum; done)"
echo "ETH=$(ip -o addr show eth0 2>/dev/null | awk '/inet /{printf "%s ",$4}')"
EOF
)"
if [ -z "$OUT" ]; then
  bad "ssh на RV не вдався (пароль/ключ). Для автоматизації: ops/setup_keys.sh"
  exit 3
fi

MODE=$(printf '%s\n' "$OUT" | sed -n 's/^MODE=//p')
RTSPPID=$(printf '%s\n' "$OUT" | sed -n 's/^RTSPPID=//p')
TCPPID=$(printf '%s\n' "$OUT" | sed -n 's/^TCPPID=//p')
CAMDEV=$(printf '%s\n' "$OUT" | sed -n 's/^CAMDEV=//p')
ETH=$(printf '%s\n' "$OUT" | sed -n 's/^ETH=//p')

[ -n "$ETH" ] && ok "eth0: $ETH" || bad "eth0 без адреси"
info "режим виходу: ${MODE:-?}"

if [ -n "$CAMDEV" ]; then ok "камера Seek підключена (devnum=$CAMDEV)"
else bad "камери Seek немає (VID 3474) → sender не стартує; перевір хаб+живлення+землю"; fi

if [ -n "$RTSPPID" ]; then ok "rv1106_rtsp працює (pid $RTSPPID)"
elif [ -n "$TCPPID" ]; then warn "працює rv1106_sender (TCP, pid $TCPPID) — для Beam потрібен режим rtsp"
else bad "sender не запущений"; fi

# 3) RTSP доступний із ноута?
URL="rtsp://${RV_HOST}:${RTSP_PORT}${RTSP_PATH}"
if command -v ffprobe >/dev/null 2>&1; then
  if timeout 8 ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name,width,height \
        -of default=nw=1 "$URL" >/tmp/rvprobe.$$ 2>/dev/null; then
    ok "RTSP живий: $URL"; sed 's/^/    /' /tmp/rvprobe.$$
  else
    bad "RTSP не відповідає: $URL (камера впала? порт зайнятий rkipc? не той шлях?)"
  fi
  rm -f /tmp/rvprobe.$$
else
  warn "ffprobe нема на ноуті — пропускаю перевірку RTSP (постав ffmpeg або глянь ffplay вручну)"
fi
