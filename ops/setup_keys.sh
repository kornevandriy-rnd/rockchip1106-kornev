#!/usr/bin/env bash
# ops/setup_keys.sh — поставити SSH-ключ на борт і землю, щоб агенти ходили
# БЕЗ пароля (обов'язково для автоматизації). Запустити ОДИН раз із ноута.
# Запитає пароль плат по разу (RV: luckfox, RK: armsom/дефолт образу).
set -u
. "$(dirname "$0")/lib.sh"
hdr "Налаштування SSH-ключів для Dev/Ops-агентів"

# згенерувати ключ, якщо нема
if [ ! -f "$HOME/.ssh/id_ed25519" ] && [ ! -f "$HOME/.ssh/id_rsa" ]; then
  info "генерую ключ ~/.ssh/id_ed25519"
  ssh-keygen -t ed25519 -N "" -f "$HOME/.ssh/id_ed25519"
fi

copy() { # $1=user@host
  if command -v ssh-copy-id >/dev/null 2>&1; then
    ssh-copy-id -o StrictHostKeyChecking=accept-new "$1" && ok "ключ на $1" || bad "не вдалось на $1"
  else
    warn "нема ssh-copy-id — копіюю вручну на $1"
    KEY=$(cat "$HOME"/.ssh/id_*.pub | head -1)
    ssh -o StrictHostKeyChecking=accept-new "$1" \
      "mkdir -p ~/.ssh && echo '$KEY' >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys" \
      && ok "ключ на $1" || bad "не вдалось на $1"
  fi
}

info "RV1106 → ${RV_USER}@${RV_HOST} (пароль luckfox)"
copy "${RV_USER}@${RV_HOST}"
info "RK3588 → ${RK_USER}@${RK_HOST} (пароль образу)"
copy "${RK_USER}@${RK_HOST}"

hdr "Перевірка (має зайти без пароля)"
rv_ssh 'echo RV ok: $(hostname)' 2>/dev/null && ok "RV без пароля" || warn "RV ще просить пароль"
rk_ssh 'echo RK ok: $(hostname)' 2>/dev/null && ok "RK без пароля" || warn "RK ще просить пароль"
