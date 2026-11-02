# ops/lib.sh — спільні параметри й хелпери для Dev/Ops-агентів проекту.
# Підключати в кожному скрипті:  . "$(dirname "$0")/lib.sh"
#
# Усі адреси/порти — ПАРАМЕТРИ з дефолтами (бо в польових умовах вони «плавали»).
# Перевизначення — через env, напр.:  RK_HOST=192.168.1.2 ops/rk_check.sh
#
# Виконувати ЗВІДКИ Є мережевий доступ до плат (термінальний Claude на ноуті).

# ── Параметри ───────────────────────────────────────────────────────────────
: "${RV_HOST:=192.168.1.10}"      # RV1106 (борт), eth0 у мережі Beam
: "${RK_HOST:=192.168.0.162}"     # RK3588/BPI (керування; WiFi-IP стабільний)
: "${BEAM_RX:=192.168.1.21}"      # наземний модуль Beam (кому йде keepalive)
: "${GCS_IP:=192.168.1.2}"        # адреса наземного приймача (куди Beam пушить відео)
: "${VIDEO_PORT:=5700}"           # RTP/UDP-порт відео на землі
: "${RTSP_PORT:=554}"             # RTSP на борту
: "${RTSP_PATH:=/stream=0}"       # шлях RTSP на борту
: "${RV_USER:=root}"              # логін борту (пароль luckfox)
: "${RK_USER:=armsom}"            # логін землі

# ── SSH ───────────────────────────────────────────────────────────────────────
# Для повної автоматизації постав ssh-ключі: ops/setup_keys.sh (тоді без пароля).
SSH_OPTS="${SSH_OPTS:--o ConnectTimeout=6 -o StrictHostKeyChecking=accept-new}"
rv_ssh() { ssh $SSH_OPTS "${RV_USER}@${RV_HOST}" "$@"; }
rk_ssh() { ssh $SSH_OPTS "${RK_USER}@${RK_HOST}" "$@"; }

# ── Вивід ─────────────────────────────────────────────────────────────────────
if [ -t 1 ]; then R=$'\e[31m'; G=$'\e[32m'; Y=$'\e[33m'; B=$'\e[36m'; DIM=$'\e[2m'; N=$'\e[0m'
else R=; G=; Y=; B=; DIM=; N=; fi
ok()   { printf "%s✓%s %s\n" "$G" "$N" "$*"; }
bad()  { printf "%s✗%s %s\n" "$R" "$N" "$*"; }
warn() { printf "%s!%s %s\n" "$Y" "$N" "$*"; }
info() { printf "%s·%s %s\n" "$B" "$N" "$*"; }
hdr()  { printf "\n%s== %s ==%s\n" "$B" "$*" "$N"; }

# ── Загальне ──────────────────────────────────────────────────────────────────
# ping з короткими опціями (працює на Linux-ноуті)
pingable() { ping -c1 -W2 "$1" >/dev/null 2>&1; }

show_params() {
  info "RV=${RV_USER}@${RV_HOST}  RK=${RK_USER}@${RK_HOST}  Beam_RX=${BEAM_RX}"
  info "GCS=${GCS_IP}  video=udp/${VIDEO_PORT}  rtsp=${RTSP_PORT}${RTSP_PATH}"
}
