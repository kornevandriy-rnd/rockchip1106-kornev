# RK3588 / BPI-M7 (земля) — наземний приймач Beam

Усе, щоб з нуля відтворити наземну станцію: приймає відео з Beam RX і показує на HDMI.

```
Beam RX (192.168.1.21) --RTP/UDP H.265--> BPI (192.168.1.2:5700) → ffmpeg → gst → HDMI
керування — з ноута по WiFi:  ssh armsom@192.168.0.162
```

## Вміст папки
| Файл | Що це |
|---|---|
| `setup.sh` | **одноразове** налаштування (sysctl, мережа, ffmpeg, служба) |
| `thermal_receiver.sh` | приймач: ffmpeg(RTP+RTCP) + keepalive-пінг + софт-декод → HDMI |
| `thermal-receiver.service` | systemd-автозапуск приймача |
| `99-beam.conf` | sysctl: буфер сокета + ping без root |

---

## 0. Передумови
- Debian 12 на BPI-M7 (`armsom-sige5`), user `armsom` з **passwordless sudo**.
- GStreamer з Rockchip MPP (штатно на образі).
- WiFi для керування (ssh `armsom@192.168.0.162`) + HDMI-монітор.
- Кабель Beam RX у **один** з Ethernet-портів (end0 або end1).

## 1. Встановлення (на платі)
```sh
# скопіювати цю папку на BPI (напр. по scp з ноута) і:
cd RK3588
bash setup.sh
```
`setup.sh` зробить усе: sysctl, nmcli-профілі (`192.168.1.2` на порту з кабелем),
статичний `ffmpeg` у `~/bin`, встановить і увімкне службу.

## 2. Підібрати робочий синк (важливо, один раз)
На цьому образі відео звичайних (софтових) кадрів малює **не кожен** синк:
- `rkximagesink` — тільки апаратні кадри (для нашого софт-декоду **чорний**);
- `ximagesink` — падає на XInput;
- **`xvimagesink` / `glimagesink`** — малюють звичайні кадри. Перевір тестом:
```sh
export DISPLAY=:0 XAUTHORITY=/home/armsom/.Xauthority
timeout 7 gst-launch-1.0 videotestsrc ! videoconvert ! xvimagesink   # смуги є? → бери його
timeout 7 gst-launch-1.0 videotestsrc ! videoconvert ! glimagesink   # інакше цей
```
Той, що показав смуги, пропиши службі:
```sh
sudo systemctl edit thermal-receiver
# додати:
#   [Service]
#   Environment=SINK=xvimagesink        # або glimagesink
sudo systemctl restart thermal-receiver
```
(Скрипт за замовчуванням бере `SINK=xvimagesink`.)

## 3. Запуск і перевірка
```sh
sudo systemctl restart thermal-receiver
sudo systemctl status thermal-receiver
```
Відео має зʼявитись на HDMI. Ручна перевірка прийому:
```sh
export DISPLAY=:0 XAUTHORITY=/home/armsom/.Xauthority
FF=~/bin/ffmpeg
( sudo ping -i 1 192.168.1.21 >/dev/null 2>&1 & )
printf 'v=0\no=- 0 0 IN IP4 192.168.1.2\ns=beam\nc=IN IP4 192.168.1.2\nt=0 0\nm=video 5700 RTP/AVP 96\na=rtpmap:96 H265/90000\n' > /tmp/beam.sdp
$FF -protocol_whitelist file,udp,rtp -buffer_size 8388608 -reorder_queue_size 2000 -i /tmp/beam.sdp -an -pix_fmt yuv420p -f rawvideo - 2>/dev/null | \
  gst-launch-1.0 fdsrc ! rawvideoparse width=1280 height=1024 format=i420 framerate=25/1 ! videoconvert ! xvimagesink sync=false
```

---

## 4. Ключові правила (щоб «без проблем»)
1. **Порт із кабелем.** Адреса `192.168.1.2` має бути ЛИШЕ на порту, де кабель
   (LOWER_UP). На обох одразу — конфлікт і `Destination Host Unreachable`.
   Знайти: `ip -o link show end0|end1 | grep -oE "LOWER_UP|NO-CARRIER"`.
2. **Keepalive-пінг безперервний.** Поки приймач пінгує `192.168.1.21`, потік іде;
   пінг стих — Beam глушить. (Скрипт тримає його сам.)
3. **RTCP.** Приймати треба через `ffmpeg` (він шле RTCP і заводить Beam), а не
   голим `gst udpsrc`.
4. **Втрати радіо** → софт-декод (ffmpeg). Апаратний `mppvideodec` дасть чорне на
   «битому» потоці. Коли сигнал чистий (max power + Dynamic power на Beam) — можна
   повернути HW-декод.
5. **ffmpeg — у `~/bin`, не в `/tmp`** (/tmp чиститься на ребуті).

## 5. Діагностика
```sh
journalctl -u thermal-receiver -e
sudo tcpdump -n -i <порт> udp port 5700 -c 5     # чи летять пакети
pgrep -af "ping -i 1 192.168.1.21"               # чи живий keepalive
```
| Симптом | Причина | Лік |
|---|---|---|
| 0 пакетів | адреса не на порту з кабелем | адресу на LOWER_UP-порт |
| вікно чорне | синк rkximagesink / не той синк | xvimagesink / glimagesink |
| відео зникло за N с | нема keepalive-пінгу | тримати ping (служба це робить) |
| «каша» | втрати радіо | софт-декод (є) / потужність Beam |
