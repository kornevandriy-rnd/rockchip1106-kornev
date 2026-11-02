#!/usr/bin/env bash
# ops/rk_check.sh — стан наземки RK3588/BPI: порт з кабелем, IP, keepalive,
# пакети відео, приймач/служба. Запускати з ноута (ssh до RK по керуванню).
# env: RK_HOST, RK_USER, GCS_IP, BEAM_RX, VIDEO_PORT
set -u
. "$(dirname "$0")/lib.sh"

hdr "RK3588/BPI (земля) — ${RK_USER}@${RK_HOST}"

if ! pingable "$RK_HOST"; then
  bad "ping $RK_HOST — не відповідає (плата off / host key / інший IP)"
  info "якщо перепрошили — знайди IP: nmap -sn <підмережа>; або HDMI-консоль"
  exit 2
fi
ok "ping $RK_HOST"

# усе одним ssh — швидше й атомарно
OUT="$(rk_ssh 'bash -s' <<EOF 2>/dev/null
GCS_IP="$GCS_IP"; BEAM_RX="$BEAM_RX"; VIDEO_PORT="$VIDEO_PORT"
for p in end0 end1; do echo "CARR:\$p=\$(cat /sys/class/net/\$p/carrier 2>/dev/null || echo NA)"; done
echo "IPPORT=\$(ip -o addr show | awk -v ip="\$GCS_IP" '\$0 ~ ip {print \$2}')"
echo "IPCOUNT=\$(ip -o addr show | grep -c "\$GCS_IP/")"
echo "KEEPALIVE=\$(pgrep -af "ping .*\$BEAM_RX" | head -1)"
echo "RECV=\$(pgrep -af "ffmpeg|gst-launch-1.0" | head -1)"
echo "SVC=\$(systemctl is-active thermal-receiver 2>/dev/null)"
echo "BEAMPING=\$(ping -c1 -W2 \$BEAM_RX >/dev/null 2>&1 && echo ok || echo no)"
EOF
)"
if [ -z "$OUT" ]; then
  bad "ssh на RK не вдався (пароль/ключ/перепрошивка). Ключі: ops/setup_keys.sh"
  exit 3
fi
getv(){ printf '%s\n' "$OUT" | sed -n "s/^$1=//p"; }

# порт із кабелем
CARRIER_PORT=""
for line in $(printf '%s\n' "$OUT" | sed -n 's/^CARR://p'); do
  p=${line%%=*}; v=${line##*=}
  if [ "$v" = 1 ]; then ok "кабель у $p (carrier)"; CARRIER_PORT=$p; else info "$p: без лінку"; fi
done
[ -z "$CARRIER_PORT" ] && bad "жоден порт не має carrier — кабель Beam не встромлений?"

IPPORT=$(getv IPPORT); IPCOUNT=$(getv IPPORT >/dev/null; printf '%s' "$(getv IPCOUNT)")
if [ -n "$IPPORT" ]; then
  if [ -n "$CARRIER_PORT" ] && [ "$IPPORT" != "$CARRIER_PORT" ]; then
    bad "$GCS_IP на '$IPPORT', а кабель у '$CARRIER_PORT' → мимо! (fix_rk_ip.sh)"
  else ok "$GCS_IP на $IPPORT"; fi
  [ "${IPCOUNT:-1}" -gt 1 ] 2>/dev/null && bad "$GCS_IP на КІЛЬКОХ портах → конфлікт адрес (fix_rk_ip.sh)"
else
  bad "$GCS_IP не піднята на жодному порту (fix_rk_ip.sh)"
fi

[ "$(getv BEAMPING)" = ok ] && ok "Beam RX $BEAM_RX доступний" || bad "Beam RX $BEAM_RX недоступний"
[ -n "$(getv KEEPALIVE)" ] && ok "keepalive-пінг живий" || bad "keepalive-пінг НЕ працює (Beam глушитиме потік)"
[ -n "$(getv RECV)" ] && ok "приймач працює: $(getv RECV | cut -c1-60)…" || bad "приймач (ffmpeg/gst) не запущений"
case "$(getv SVC)" in active) ok "служба thermal-receiver active";; "") info "служби thermal-receiver нема (запуск вручну?)";; *) warn "служба thermal-receiver: $(getv SVC)";; esac

# пакети відео на порту з кабелем
if [ -n "$CARRIER_PORT" ]; then
  info "ловлю пакети udp/$VIDEO_PORT на $CARRIER_PORT (5с)…"
  PKTS=$(rk_ssh "sudo timeout 5 tcpdump -n -i $CARRIER_PORT udp port $VIDEO_PORT 2>/dev/null | grep -c '> '" 2>/dev/null)
  if [ "${PKTS:-0}" -gt 0 ] 2>/dev/null; then ok "відео-пакети йдуть ($PKTS за 5с)"
  else bad "відео-пакетів немає → Beam не шле на RK (keepalive/RTCP/порт/сесія)"; fi
fi
