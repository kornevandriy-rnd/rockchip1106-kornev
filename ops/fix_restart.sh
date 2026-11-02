#!/usr/bin/env bash
# ops/fix_restart.sh — перезапустити приймач на землі або sender на борту.
#   ops/fix_restart.sh rk   # RK: перезапуск служби thermal-receiver (приймач+keepalive)
#   ops/fix_restart.sh rv   # RV: перезапуск S99thermal (sender)
set -u
. "$(dirname "$0")/lib.sh"
TARGET="${1:-}"

case "$TARGET" in
  rk)
    hdr "FIX: перезапуск приймача на RK"
    rk_ssh 'bash -s' <<'EOF'
if systemctl list-unit-files 2>/dev/null | grep -q thermal-receiver; then
  sudo systemctl restart thermal-receiver && echo "служба thermal-receiver перезапущена"
  sleep 3; systemctl is-active thermal-receiver
else
  echo "служби нема — підніми приймач вручну через ~/thermal_receiver.sh"
  echo "або встанови службу: delivery/RK3588/setup.sh"
fi
EOF
    ;;
  rv)
    hdr "FIX: перезапуск sender на RV"
    rv_ssh '/etc/init.d/S99thermal restart; sleep 3; /etc/init.d/S99thermal status'
    ;;
  *)
    bad "вкажи ціль: rk або rv"; echo "  ops/fix_restart.sh rk"; echo "  ops/fix_restart.sh rv"; exit 2 ;;
esac
