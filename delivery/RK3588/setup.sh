#!/bin/bash
# setup.sh — одноразове налаштування наземного приймача на BPI-M7 (RK3588).
# Запускати НА платі, з цієї папки:  bash setup.sh
# Робить: sysctl-тюнінг, nmcli-профілі (192.168.1.2 на порту з кабелем),
# статичний ffmpeg у ~/bin, встановлення й увімкнення systemd-служби.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "== 1/5 sysctl (буфер + ping без root) =="
sudo cp "$HERE/99-beam.conf" /etc/sysctl.d/99-beam.conf
sudo sysctl --system >/dev/null

echo "== 2/5 nmcli-профілі beam-e0/beam-e1 (192.168.1.2) =="
sudo nmcli con add type ethernet ifname end0 con-name beam-e0 \
     ipv4.method manual ipv4.addresses 192.168.1.2/24 ipv6.method disabled 2>/dev/null || true
sudo nmcli con add type ethernet ifname end1 con-name beam-e1 \
     ipv4.method manual ipv4.addresses 192.168.1.2/24 ipv6.method disabled 2>/dev/null || true
# активуємо ЛИШЕ порт із кабелем (LOWER_UP), інакший вимикаємо (щоб не було конфлікту адрес)
if ip -o link show end0 | grep -q LOWER_UP; then
    sudo nmcli con up beam-e0; sudo nmcli con down beam-e1 2>/dev/null || true
    echo "   кабель у end0"
elif ip -o link show end1 | grep -q LOWER_UP; then
    sudo nmcli con up beam-e1; sudo nmcli con down beam-e0 2>/dev/null || true
    echo "   кабель у end1"
else
    echo "   !!! кабель Beam не знайдено (ні end0, ні end1 не LOWER_UP) — встроми і перезапусти"
fi

echo "== 3/5 статичний ffmpeg у ~/bin =="
mkdir -p "$HOME/bin"
if [ ! -x "$HOME/bin/ffmpeg" ]; then
    cd /tmp
    wget -q https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz -O ff.tar.xz
    tar xf ff.tar.xz
    cp "$(ls -d /tmp/ffmpeg-*-static | head -1)/ffmpeg" "$HOME/bin/ffmpeg"
    chmod +x "$HOME/bin/ffmpeg"
fi
"$HOME/bin/ffmpeg" -version | head -1

echo "== 4/5 приймач + служба =="
cp "$HERE/thermal_receiver.sh" "$HOME/thermal_receiver.sh"
chmod +x "$HOME/thermal_receiver.sh"
sudo cp "$HERE/thermal-receiver.service" /etc/systemd/system/thermal-receiver.service
sudo systemctl daemon-reload
sudo systemctl enable thermal-receiver.service

echo "== 5/5 готово =="
echo "Перевір робочий синк (див. README) і, за потреби, постав SINK у службі."
echo "Запуск:  sudo systemctl restart thermal-receiver  ;  відео на HDMI."
