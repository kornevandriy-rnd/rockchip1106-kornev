#!/usr/bin/env bash
# ops/fix_rk_ip.sh — поставити GCS_IP (192.168.1.2) на ТОЙ порт RK, де кабель,
# і прибрати з іншого (щоб не було конфлікту адрес). Кернел BPI без bridge,
# тому прив'язуємо до конкретного порту через nmcli-профілі beam-e0/beam-e1.
set -u
. "$(dirname "$0")/lib.sh"
hdr "FIX: адреса $GCS_IP на порт із кабелем (RK)"

rk_ssh "bash -s" <<EOF
set -e
GCS_IP="$GCS_IP"
cp=""; for p in end0 end1; do [ "\$(cat /sys/class/net/\$p/carrier 2>/dev/null)" = 1 ] && cp=\$p; done
[ -z "\$cp" ] && { echo "нема порту з кабелем (carrier) — встроми Beam RX"; exit 2; }
echo "кабель у \$cp"
# профілі (створити, якщо нема)
sudo nmcli con add type ethernet ifname end0 con-name beam-e0 ipv4.method manual ipv4.addresses \$GCS_IP/24 ipv6.method disabled 2>/dev/null || true
sudo nmcli con add type ethernet ifname end1 con-name beam-e1 ipv4.method manual ipv4.addresses \$GCS_IP/24 ipv6.method disabled 2>/dev/null || true
# підняти лише порт із кабелем, інший — вимкнути
if [ "\$cp" = end0 ]; then sudo nmcli con up beam-e0; sudo nmcli con down beam-e1 2>/dev/null || true
else                       sudo nmcli con up beam-e1; sudo nmcli con down beam-e0 2>/dev/null || true; fi
echo "---"; ip -o addr show | grep "\$GCS_IP" || echo "УВАГА: адреса не піднялась"
EOF
rc=$?
[ $rc -eq 0 ] && ok "готово — перевір: ops/diagnose.sh" || bad "не вдалось (rc=$rc)"
exit $rc
