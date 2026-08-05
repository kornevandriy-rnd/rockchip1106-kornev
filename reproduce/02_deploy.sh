#!/bin/sh
# 02_deploy.sh — залити зібраний sender на плату (/root).
# Env: BOARD_IP (деф. 192.168.50.2)
set -e
: "${BOARD_IP:=192.168.50.2}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/rv1106_sender/rv1106_sender"

[ -f "$BIN" ] || { echo "!!! Немає бінара $BIN — спершу ./reproduce/01_build.sh"; exit 1; }

echo "== прибиваю старий процес на платі (щоб файл не був зайнятий) =="
ssh root@"$BOARD_IP" 'pkill -9 rv1106_sender 2>/dev/null; sleep 1' || true

echo "== scp -> /root/rv1106_sender =="
scp "$BIN" root@"$BOARD_IP":/root/
ssh root@"$BOARD_IP" 'chmod +x /root/rv1106_sender'
echo "ГОТОВО. Далі: ./reproduce/03_run_board.sh"
