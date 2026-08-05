#!/bin/sh
# 03_run_board.sh — запустити sender НА ПЛАТІ (блокує термінал; Ctrl-C = стоп).
# Знаходить YUYV-ноду динамічно (номер /dev/videoN плаває), звільняє енкодер,
# ставить LD_LIBRARY_PATH. Аргументи sender можна перевизначити через SENDER_ARGS.
# Env: BOARD_IP (деф. 192.168.50.2), SENDER_ARGS (деф. "--port 5000")
: "${BOARD_IP:=192.168.50.2}"
: "${SENDER_ARGS:=--port 5000}"

ssh root@"$BOARD_IP" "export LD_LIBRARY_PATH=/oem/usr/lib; \
  pkill -9 rkipc 2>/dev/null; pkill -9 ispserver 2>/dev/null; pkill -9 rv1106_sender 2>/dev/null; sleep 1; \
  CAM=\$(for v in /dev/video*; do v4l2-ctl -d \"\$v\" --list-formats 2>/dev/null | grep -q YUYV && { echo \"\$v\"; break; }; done); \
  echo \"нода камери = \${CAM:-НЕ ЗНАЙДЕНО}\"; \
  [ -n \"\$CAM\" ] && /root/rv1106_sender --device \"\$CAM\" $SENDER_ARGS || echo 'камера не на шині — перевір lsusb | grep 3474 і живлення/землю'"
