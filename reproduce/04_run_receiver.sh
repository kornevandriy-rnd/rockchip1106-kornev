#!/bin/sh
# 04_run_receiver.sh — запустити приймач НА НОУТІ (ffplay під'єднається до плати).
# Env: BOARD_IP (деф. 192.168.50.2), PORT (деф. 5000)
: "${BOARD_IP:=192.168.50.2}"
: "${PORT:=5000}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/fedora_receiver/fedora_receiver"

[ -f "$BIN" ] || { echo "!!! Немає $BIN — спершу ./reproduce/01_build.sh"; exit 1; }
exec "$BIN" "$BOARD_IP" "$PORT"
