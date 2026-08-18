# Наземна станція на Banana Pi BPI-M7 (RK3588): апаратний H.265-приймач

Замість ноута приймати й декодувати стрім з RV1106 **на платі RK3588**
(Banana Pi BPI-M7 / ArmSoM Sige5) — апаратним декодером Rockchip MPP, з виводом
на HDMI. Це крок до фінальної схеми, де між RV1106 і RK3588 стоятиме цифровий
радіоканал; поки що плати з'єднані **прямим Ethernet-кабелем** (імітація «ефіру»).

```
RV1106 (sender, TCP :5000, H.265 Annex-B)
   │  прямий Ethernet-кабель  (eth0 плати ↔ end0 BPI)
   ▼
BPI-M7 / RK3588  →  gst-launch tcpclientsrc ! h265parse ! mppvideodec (HW) ! HDMI
```

Перевірено 2026-08: живе відео з тепловізора Seek 640 йде з RV1106 на монітор
BPI-M7, декодування апаратне (mppvideodec), затримка лінка плата↔BPI ~0.5 мс.

---

## 0. Що на руках

| | Значення (перевірене) |
|---|---|
| BPI-M7 ОС | Debian 12 (bookworm), ядро `6.1.57`, aarch64 |
| hostname / user | `armsom-sige5` / `armsom` |
| Ethernet-порт BPI | `end0` (другий порт `end1` не використовуємо) |
| WiFi BPI | `wlan1`, напр. `192.168.0.162/24` (для SSH з ноута) |
| HW-декодер | `/dev/mpp_service` + `/dev/rga`, gstreamer `mppvideodec` |
| RV1106 Ethernet | `eth0`, статичний `192.168.50.2/24`, лінк `100Mbps/Full` |
| Камера | Seek `3474:43e2`, YUYV-нода `/dev/video0` |

> ⚠️ **Образ BPI «заморожений»**: усі apt-пакети held (`libav 5.1.5` тримається),
> тому **ffmpeg поставити не вийде**. Тому декодуємо через **gstreamer** —
> `gstreamer1.0-rockchip1` (дає `mppvideodec`) вже стоїть; за потреби докинути
> лише `gstreamer1.0-tools` (ставиться чисто).

## 1. Мережа: прямий лінк RV1106 ↔ BPI

RV1106 має **один** Ethernet-порт зі статичним `192.168.50.2`. Втикаємо його
кабелем прямо в `end0` BPI і даємо BPI адресу в тій самій підмережі.

На **BPI**:
```sh
sudo ip addr add 192.168.50.10/24 dev end0
sudo ip link set end0 up
sudo ping -c3 192.168.50.2          # має піти time=… ~0.5 мс
```

> 🟥 **NetworkManager стирає IP, доданий вручну через `ip addr`** (через ~хвилину
> робить DHCP-спробу і скидає адресу → `Destination Host Unreachable`).
> Щоб адреса трималась постійно — зробити її через NetworkManager:
> ```sh
> sudo nmcli con add type ethernet ifname end0 con-name rv1106 \
>      ipv4.method manual ipv4.addresses 192.168.50.10/24 ipv6.method ignore autoconnect yes
> sudo nmcli con up rv1106
> ip -br addr show end0     # має стало показувати 192.168.50.10/24
> ```
> Це перевірено: після `nmcli`-профілю адреса **тримається** й не зникає посеред
> роботи (без нього NM стирав її ~щохвилини — це давало «плата не відповідає»).

Після цього RV1106 доступний **з BPI** по SSH (пароль плати `luckfox`):
```sh
ssh root@192.168.50.2 'echo OK'
```
Керуємо платою **з BPI**, а не з ноута — кабель тепер у BPI, тож із ноута плата
не видна (це нормально).

## 2. Наземна сторона: перевірка HW-декодера (без плати)

Самотест «HW-енкод → HW-декод → екран» прямо на BPI:
```sh
DISPLAY=:0 gst-launch-1.0 videotestsrc num-buffers=150 \
  ! video/x-raw,format=NV12,width=1280,height=1024,framerate=30/1 \
  ! mpph265enc ! h265parse ! mppvideodec ! videoconvert ! autovideosink sync=false
```
Має відкритись вікно з тестовою картинкою → декодер і вивід на HDMI живі.

## 3. Запуск живого стріму (2 термінали)

**Термінал 1 — sender на платі** (з BPI по SSH, пароль `luckfox`):
```sh
ssh root@192.168.50.2 'export LD_LIBRARY_PATH=/oem/usr/lib; \
  pkill -9 rkipc 2>/dev/null; pkill -9 ispserver 2>/dev/null; pkill -9 rv1106_sender 2>/dev/null; sleep 1; \
  /root/rv1106_sender --device /dev/video0 --port 5000'
```
Чекай `Camera 640x512 YUYV ... HEVC 8000 kbit/s`. Далі sender **навмисне висить**,
чекаючи приймача (`accept`) — це не баг.

**Термінал 2 — приймач на BPI** (власний екран BPI або друга SSH-сесія в BPI):
```sh
DISPLAY=:0 gst-launch-1.0 tcpclientsrc host=192.168.50.2 port=5000 \
  ! h265parse ! mppvideodec \
  ! queue leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 \
  ! rkximagesink sync=false
```
> 🟢 **Низька затримка / рівні 60 fps.** НЕ вставляй `videoconvert` — він
> конвертує NV12 на CPU (1280×1024@60), стає вузьким місцем, дає джиттер, TCP
> підпирає sender → на платі стрибає `Pipeline fps` і сиплються `V4L2 dropped
> frames`. NV12 з `mppvideodec` треба вести **прямо в апаратний сінк**
> (`rkximagesink`; якщо його нема — `glimagesink sync=false`), а `queue
> leaky=downstream max-size-buffers=1` тримає лише найсвіжіший кадр. З цим на
> платі стабільні `60.0 fps, dropped 0`.

Щойно Т2 під'єднається → у Т1 `Receiver connected` + `Pipeline: … fps`, а на
моніторі BPI — тепловізійне відео 1280×1024, декодоване апаратно.

Готовий скрипт-обгортка: [`05_run_bpi_receiver.sh`](05_run_bpi_receiver.sh).

---

## Пройдені граблі (щоб не наступати вдруге)

| Симптом | Причина / рішення |
|---|---|
| `end0 does not exist`, промпт `kornev@…` | команди вводились **на ноуті**, а не в BPI. Спершу `ssh armsom@192.168.0.162`, дочекатись промпта `armsom@armsom-sige5`, і тільки тоді вводити |
| ping з BPI був ОК, потім `Destination Host Unreachable` | NetworkManager стер ручний IP на `end0` → повернути (розд.1) або зробити постійним через `nmcli` |
| `ping: socket: Operation not permitted` | ping без прав на raw-socket → запускати `sudo ping …` |
| плата пропала **саме коли встромив камеру**, `end0` показує `LOWER_UP`, але ping мовчить | плата в ребут-лупі від камери: недоживлення / плаваюча земля. Камера — **тільки через живлений хаб**, і **спільна земля** GND плати+хаба+камери. Без цього стрім не поїде (головний блокер) |
| `lsusb` показує лише кореневі хаби `1d6b:*` | камера фізично не на шині — воткнути через живлений хаб; шукати `ID 3474:…` |
| на платі стрибає `Pipeline fps` (7↔60) + багато `V4L2 dropped`, велика затримка | приймач із `videoconvert` (CPU) не встигає → TCP підпирає sender. Прибрати `videoconvert`, вести NV12 прямо в `rkximagesink`/`glimagesink` + `queue leaky=downstream max-size-buffers=1`. Стає `60.0 fps, dropped 0` |
| приймач: `Failed to connect … 拒绝连接 / Connection refused` | плата жива, але sender **не запущений** — спершу Т1 (sender), дочекатись `Listening on TCP port 5000`, тоді Т2 |
| приймач: `Failed to connect … 连接超时 / timeout` | BPI загубив IPv4 на `end0` (NM) АБО плата впала — перевір `ip -br addr show end0` і `ping`; IP зробити постійним через `nmcli` (розд.1) |
| sender «завис» після рядка `Camera … HEVC …` | нормально: чекає приймача. Запустити Т2 — піде `Receiver connected` |
| ffmpeg на BPI не ставиться (пакети held) | не боротись з apt — декодувати через gstreamer `mppvideodec` |

## Наступний крок

Замінити прямий Ethernet-кабель цифровим радіоканалом (Sine.video Beam):
RV1106 → радіо-Tx … радіо-Rx → BPI-M7. Для цього sender-у, ймовірно, доведеться
перейти з TCP на UDP/RTP/SRT. Питання до виробника радіо і план інтеграції —
[`../docs/RADIO_LINK_SINE.md`](../docs/RADIO_LINK_SINE.md).
