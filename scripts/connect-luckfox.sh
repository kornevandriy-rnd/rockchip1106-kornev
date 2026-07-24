#!/usr/bin/env bash
#
# connect-luckfox.sh — під'єднання Fedora-хоста до Luckfox Pico Ultra по USB-RNDIS.
#
# Плата віддає мережу через USB (RNDIS gadget). На хості з'являється новий
# ethernet-інтерфейс (драйвер rndis_host / cdc_ether). Скрипт знаходить його,
# вішає статичну IP 172.32.0.100/16 через NetworkManager і перевіряє зв'язок.
#
#   Плата:  172.32.0.93    (root / luckfox)
#   Хост:   172.32.0.100/16
#
# Використання:
#   ./connect-luckfox.sh            # авто-детект інтерфейсу + підняти з'єднання
#   ./connect-luckfox.sh <iface>    # вказати інтерфейс вручну (напр. enp0s20f0u1)
#   ./connect-luckfox.sh --ssh      # після підняття одразу зайти по SSH
#   ./connect-luckfox.sh --down      # прибрати з'єднання
#
set -euo pipefail

BOARD_IP="172.32.0.93"
HOST_ADDR="172.32.0.100/16"
CON_NAME="luckfox"
SSH_USER="root"

log()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n'  "$*" >&2; }
die()  { printf '\033[1;31m[x]\033[0m %s\n'  "$*" >&2; exit 1; }

IFACE=""
DO_SSH=0
DO_DOWN=0
for arg in "$@"; do
  case "$arg" in
    --ssh)     DO_SSH=1 ;;
    --down)    DO_DOWN=1 ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*)        die "Невідомий прапорець: $arg" ;;
    *)         IFACE="$arg" ;;
  esac
done

if [ "$DO_DOWN" = 1 ]; then
  log "Прибираю з'єднання '$CON_NAME'…"
  sudo nmcli connection down "$CON_NAME" 2>/dev/null || true
  sudo nmcli connection delete "$CON_NAME" 2>/dev/null || true
  exit 0
fi

# --- 1. Знайти USB-RNDIS інтерфейс ------------------------------------------
detect_iface() {
  # Найнадійніше — за драйвером USB-ethernet gadget.
  for path in /sys/class/net/*; do
    [ -e "$path/device/driver" ] || continue
    drv=$(basename "$(readlink -f "$path/device/driver")")
    case "$drv" in
      rndis_host|cdc_ether|cdc_ncm) basename "$path"; return 0 ;;
    esac
  done
  # Фолбек — за назвою (USB-ethernet: enx… або …u<N>).
  for path in /sys/class/net/*; do
    name=$(basename "$path")
    [ "$name" = "lo" ] && continue
    case "$name" in
      enx*|*u[0-9]*) echo "$name"; return 0 ;;
    esac
  done
  return 1
}

if [ -z "$IFACE" ]; then
  log "Автопошук USB-RNDIS інтерфейсу…"
  IFACE=$(detect_iface) || die "Не знайшов RNDIS-інтерфейс. Встав USB-C плати і перевір:
      nmcli device status
      dmesg | grep -iE 'rndis|cdc_ether|usb0' | tail"
fi
[ -e "/sys/class/net/$IFACE" ] || die "Інтерфейс '$IFACE' не існує."
log "Інтерфейс плати: $IFACE"

# --- 2. Підняти статичну IP через NetworkManager ----------------------------
if nmcli -t -f NAME connection show 2>/dev/null | grep -qx "$CON_NAME"; then
  log "З'єднання '$CON_NAME' уже є — оновлюю інтерфейс і піднімаю."
  sudo nmcli connection modify "$CON_NAME" \
       connection.interface-name "$IFACE" \
       ipv4.method manual ipv4.addresses "$HOST_ADDR"
else
  log "Створюю з'єднання '$CON_NAME' на $IFACE ($HOST_ADDR)."
  sudo nmcli connection add type ethernet ifname "$IFACE" con-name "$CON_NAME" \
       ipv4.method manual ipv4.addresses "$HOST_ADDR"
fi
sudo nmcli connection up "$CON_NAME"

# --- 3. Перевірити зв'язок ---------------------------------------------------
log "Пінгую плату $BOARD_IP…"
if ping -c3 -W2 "$BOARD_IP"; then
  log "Плата відповідає ✅"
else
  warn "Плата не пінгується. Перевір кабель / режим USB і 'nmcli device status'."
  exit 1
fi

# --- 4. SSH (опційно) --------------------------------------------------------
SSH_OPTS="-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAuthentication=no"
if [ "$DO_SSH" = 1 ]; then
  log "Заходжу по SSH (пароль: luckfox)…"
  exec ssh $SSH_OPTS "$SSH_USER@$BOARD_IP"
fi

cat <<EOF

Готово ✅  Далі — зайти на плату (пароль: luckfox):
  ssh $SSH_OPTS $SSH_USER@$BOARD_IP

Або зібрати інвентар плати одним рядком (без копіювання файлу):
  ssh $SSH_OPTS $SSH_USER@$BOARD_IP 'sh -s' < scripts/board-probe.sh
EOF
