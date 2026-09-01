# Наземна плата BPI-M7 — автоприймач через Sine.video Beam (hands-free)

Плата приймає відео з **цифрового радіоканалу Beam** і виводить на HDMI з
**апаратним H.265-декодом**. Тракт:

```
RV1106 (IP-камера, RTSP) → Beam TX ))) радіо ((( Beam RX → BPI-M7 (RK3588) → HDMI
                                                   192.168.1.21 → 192.168.1.2:5700 (end1)
```

Beam **не віддає RTSP на землі** — він штовхає **RTP/UDP H.265** (payload 96) на
`192.168.1.2:5700`. Порт `end1` BPI під'єднаний до наземного Beam (RX). Деталі й
факти про канал — `../docs/RADIO_LINK_SINE.md`.

## Що ставимо

- `thermal_receiver.sh` → у `~/` на BPI. Keepalive до Beam RX + самовідновний
  gstreamer-приймач (`udpsrc :5700` → `rtpjitterbuffer` → `rtph265depay` →
  `mppvideodec` (HW) → `rkximagesink` HDMI).
- `thermal-receiver.service` → systemd-служба (стартує після графічної сесії,
  рестарт при падінні).

## Одноразова підготовка (персистентні передумови)

Ці три речі ставляться **один раз** (з правами root; далі служба їх не потребує):

```sh
# 1) статичний IP на порту, у який встромлений Beam RX (end1)
sudo nmcli con add type ethernet ifname end1 con-name beam-ground ip4 192.168.1.2/24
sudo nmcli con up beam-ground

# 2) тюнінг: великий буфер сокета (густий потік дрібних пакетів) + ping без root
#    (keepalive-пінг у службі має працювати від користувача armsom)
printf 'net.core.rmem_max=16777216\nnet.ipv4.ping_group_range=0 2147483647\n' \
  | sudo tee /etc/sysctl.d/99-beam.conf
sudo sysctl --system
```

> `end1` — порт, де лінк живий (`ip -o link show end1` → `LOWER_UP`). Якщо Beam
> встромлений в `end0`, підстав його в командах вище.

## Встановлення служби (на BPI)

Скопіювати скрипт і службу (наприклад, з ноута, який у тій самій WiFi-мережі
керування, `scp` на WiFi-IP плати; або створити прямо на BPI через `cat > … <<'EOF'`):

```sh
scp bpi/thermal_receiver.sh      armsom@<wifi-ip-bpi>:~/thermal_receiver.sh
scp bpi/thermal-receiver.service armsom@<wifi-ip-bpi>:/tmp/thermal-receiver.service
ssh armsom@<wifi-ip-bpi> 'chmod +x ~/thermal_receiver.sh; \
  sudo mv /tmp/thermal-receiver.service /etc/systemd/system/; \
  sudo systemctl daemon-reload; sudo systemctl enable --now thermal-receiver.service'
```

Перевірка: `systemctl status thermal-receiver.service`, лог: `journalctl -u thermal-receiver -e`.

## Робочий процес (hands-free)

1. Увімкнути борт (RV1106 сам віддає RTSP; Beam TX його тягне).
2. Увімкнути обидва модулі Beam.
3. Увімкнути BPI-M7 (Beam RX у `end1`, HDMI-монітор).
4. Відео зʼявляється на екрані само — жодних команд.

## Керування вручну / діагностика

```sh
sudo systemctl restart thermal-receiver     # перезапустити приймач
sudo systemctl stop thermal-receiver        # зупинити
~/thermal_receiver.sh                        # запустити вручну (у графічній сесії)

# чи летять пакети відео на плату:
sudo tcpdump -n -i end1 udp port 5700 -c 5
# ручний пайплайн (для перевірки):
DISPLAY=:0 XAUTHORITY=/home/armsom/.Xauthority gst-launch-1.0 \
  udpsrc port=5700 buffer-size=4194304 \
  caps="application/x-rtp,media=video,encoding-name=H265,payload=96,clock-rate=90000" \
  ! rtpjitterbuffer latency=100 ! rtph265depay ! h265parse ! mppvideodec \
  ! queue leaky=downstream max-size-buffers=3 ! rkximagesink sync=false
```

## Часті проблеми

| Симптом | Причина | Лік |
|---|---|---|
| `0 packets` у tcpdump | адреса на не тому порту / кабель в іншому порту / ноут ще тримає 192.168.1.2 | адресу на `end1` (де `LOWER_UP`); від'єднати ноут від наземного Beam |
| відео пішло і за N с зникло | нема слухача/keepalive → Beam глушить потік | тримати службу запущеною (у ній keepalive-пінг + слухач 5700) |
| «кадр і стоп», замерзання | нема `queue leaky` перед синком → затор | пайплайн уже з `queue leaky=downstream` |
| каша/артефакти | приймач не встигає (софт-декод/малий буфер) | HW `mppvideodec` + `buffer-size=4M` (уже в скрипті) |
| `Could not create a buffer …4194304` | малий `net.core.rmem_max` | застосувати `/etc/sysctl.d/99-beam.conf` (крок 2) |

## Стара схема (архів)

Прямий тракт RV1106↔BPI по Ethernet (TCP `rv1106_sender :5000`) — див. історію
git. Поточна конфігурація — через Beam (RTP/UDP :5700), як описано вище.
