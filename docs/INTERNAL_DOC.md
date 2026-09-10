# Внутрішня документація: тепловізійний відеотракт RV1106 → Beam → RK3588

Єдиний довідник, щоб нічого не губилось: що залито на кожну плату, які файли
створені, що налаштовано, як запускати й діагностувати. Проект — стрім
**Seek UAV 640** з **Luckfox Pico Ultra (RV1106)** через цифровий радіоканал
**Sine.video Beam** на наземну станцію **Banana Pi BPI-M7 (RK3588/RK3576)**.

Дата фіксації робочого стану: **2026-09-10**.

---

## 1. Архітектура (повний тракт)

```
БОРТ (дрон):
  Seek UAV 640 (USB-UVC, YUYV 640×512)
     → RV1106: V4L2 → RGA (YUYV→NV12 + апскейл до 1280×1024) → VEPU (апаратний H.265, CBR)
     → RTSP-сервер на платі:  rtsp://192.168.1.10:554/stream=0
     → Ethernet → Beam TX (бортовий модуль)   ))) РАДІО )))

ЗЕМЛЯ:
  Beam RX (наземний модуль) → Ethernet → BPI-M7 (RK3588)
     → приймач (ffmpeg RTP/RTCP + keepalive-ping) → декод → HDMI-монітор
  Керування/налаштування — з ноутбука по WiFi.
```

Ключова особливість: **Beam — не прозорий міст.** На борту він **тягне RTSP**
з камери; на землі — **штовхає RTP/UDP** приймачу. Формат H.265 возить «як є».

---

## 2. Мережа й адреси

Мережа Beam — **192.168.1.0/24**.

| Пристрій | IP | Роль |
|---|---|---|
| RV1106 (камера) | **192.168.1.10** (eth0) + 192.168.50.2 (сервіс/налагодж.) | RTSP-сервер H.265 `rtsp://192.168.1.10:554/stream=0` |
| Beam TX (борт) | 192.168.1.11 | тягне RTSP з камери, кодує в радіо |
| Beam RX (земля) | 192.168.1.21 | приймає з радіо, **пушить RTP** на землю |
| BPI-M7 (приймач) | **192.168.1.2** (на порту з кабелем — end0 АБО end1) | приймає RTP → декод → HDMI |
| BPI-M7 (керування) | 192.168.0.162 (WiFi wlan1) | ssh-доступ, не залежить від Beam |
| Ноутбук | 192.168.0.91 (WiFi) | керування; за потреби — приймач через ffplay |

**RTP-потік на землі:** Beam RX (`192.168.1.21`) → **`192.168.1.2:5700`**,
**RTP payload type 96, H.265/HEVC, clock-rate 90000**. Пакети дрібні (~129 Б) —
Beam ріже HEVC на маленькі RTP-фрагменти (FU, NAL type 49) під радіо-MTU.
RTSP на землі **немає** (порти 554/8554 закриті).

**Beam TX (меню налаштування, борт):** Cam IP = `192.168.1.10`, потік
`rtsp://192.168.1.10:554/stream=0` (наша камера як IP-камера).

---

## 3. БОРТ — RV1106 (що залито й налаштовано)

Плата на Buildroot/uClibc, BusyBox, kernel 5.10. Доступ: `ssh root@192.168.50.2`
(BusyBox: без scp/SFTP — файли лити через `ssh … 'cat > file' <<'EOF'`).

### 3.1. Залиті бінарники (у `/root/`)
| Файл на платі | Джерело в репо | Призначення |
|---|---|---|
| `/root/rv1106_sender` | `rv1106_sender/` (файл керівника) | H.265 sender по **TCP :5000** (прямий тракт плата↔плата) |
| `/root/rv1106_rtsp` | `rv1106_rtsp/` | H.265 **RTSP-сервер** (щоб віддавати камеру в Beam) |

Обидва — той самий конвеєр (Seek → RGA → VEPU H.265 1280×1024 CBR), різниця
лише у виході (TCP-стрім vs RTSP-сервер).

### 3.2. Системні файли, змінені на платі
| Файл на платі | Що зроблено | Джерело/примітка |
|---|---|---|
| `/etc/init.d/S99thermal` | автозапуск sender'а + watchdog камери | = `board/etc/init.d/S99thermal` |
| `/root/thermal_mode` | режим виходу: `rtsp` (для Beam) або `tcp` | перемикач для S99thermal |
| `/etc/init.d/S99eth0static` | додано `192.168.1.10/24` на eth0 (поряд з 192.168.50.2) | щоб плата була в мережі Beam; персистентно |

### 3.3. Автозапуск (S99thermal) — що робить
1. зупиняє штатний `rkipc` (`RkLunch-stop.sh`) — звільняє камеру й енкодер;
2. вимикає USB-autosuspend (часта причина переенумерації Seek);
3. чекає USB-камеру (YUYV-нода, номер `/dev/videoN` «плаває») й піднімає:
   - режим `rtsp`: `rv1106_rtsp --device <нода> --port 554 --path /stream=0 --fps 25`;
   - режим `tcp`: `rv1106_sender --device <нода> --port 5000 --fps 25`;
4. **watchdog**: якщо камера переенумерувалась (змінився USB `devnum`) або sender
   впав — гасить старий процес і піднімає новий на свіжій ноді.

FPS=25 навмисне (менший USB-трафік прибирає EOVERFLOW `-75` і «disabled by hub»,
що валили USB-хаб під тривалим стрімом).

### 3.4. Камера (Seek UAV 640)
- USB-UVC, VID `3474:43e2`, формат **YUYV 640×512**.
- Потрібні: **живлений USB-хаб** + **спільна земля** з платою (інакше плата йде в
  ребут-луп / хаб відвалюється).
- Керування/діагностика: `/etc/init.d/S99thermal {start|stop|restart|status}`,
  логи `/tmp/thermal.log`, `/tmp/thermal_watchdog.log`.

### 3.5. Як зібрати бінарники (крос-компіляція)
Тулчейн `arm-rockchip830-linux-uclibcgnueabihf`, SDK Luckfox.
- `rv1106_sender/Makefile.prebuilt` — TCP-sender;
- `rv1106_rtsp/Makefile.prebuilt` — RTSP-sender (лінкує `librtsp.a` з
  `$(LUCKFOX_SDK_DIR)/media/common_algorithm/.../misc`).
Деталі: `rv1106_rtsp/README.md`, `rv1106_sender/README.mgr.md`.

---

## 4. ЗЕМЛЯ — BPI-M7 / RK3588 (що залито й налаштовано)

Debian 12, kernel 6.1, hostname `armsom-sige5`, user `armsom` (passwordless sudo).
Два Ethernet: **end0/end1**; WiFi **wlan1** (керування). GStreamer з апаратним
Rockchip MPP. Доступ: `ssh armsom@192.168.0.162`.

### 4.1. Залиті/створені файли на BPI
| Файл на платі | Джерело в репо | Призначення |
|---|---|---|
| `~/thermal_receiver.sh` | `bpi/thermal_receiver.sh` | приймач Beam + keepalive-пінг + самовідновлення |
| `/etc/systemd/system/thermal-receiver.service` | `bpi/thermal-receiver.service` | автозапуск приймача при графічній сесії |
| `/etc/sysctl.d/99-beam.conf` | (створюється разово) | `net.core.rmem_max=16777216`, `net.ipv4.ping_group_range=0 2147483647` |
| статичний `ffmpeg` (arm64) | johnvansickle static build | приймач RTP/RTCP (apt-ffmpeg на цьому образі не ставиться — конфлікт libav) |

### 4.2. Мережа на BPI (nmcli-профілі)
Кернел BPI **не підтримує bridge** — тож адресу прив'язуємо до порту з кабелем.
- `beam-e0` — статика `192.168.1.2/24` на **end0**;
- `beam-e1` — те саме на **end1**;
- **активним має бути лише ОДИН** (той, де фізично кабель, — `LOWER_UP`), інакше
  конфлікт адрес → `Destination Host Unreachable`.

Знайти порт із кабелем:
```sh
ip -o link show end0 | grep -oE "LOWER_UP|NO-CARRIER"
ip -o link show end1 | grep -oE "LOWER_UP|NO-CARRIER"
# підняти профіль на потрібному порту (приклад end1):
sudo nmcli con down beam-e0; sudo nmcli con up beam-e1
```

### 4.3. Приймач — робочий тракт  ⚠ УТОЧНИТИ ТОЧНУ КОМАНДУ
Через **втрати радіо** апаратний `mppvideodec` на «битому» H.265 чорніє, тому
приймаємо й декодуємо **ffmpeg** (толерантний до втрат), а gst лише показує.
Обов'язковий **безперервний keepalive-пінг** — без нього Beam глушить потік
(див. §5).

Робоча схема (софтовий декод + сумісний синк):
```sh
export DISPLAY=:0 XAUTHORITY=/home/armsom/.Xauthority
FF="$(ls -d /tmp/ffmpeg-*-static|head -1)/ffmpeg"
# SDP-опис RTP-потоку:
printf 'v=0\no=- 0 0 IN IP4 192.168.1.2\ns=beam\nc=IN IP4 192.168.1.2\nt=0 0\nm=video 5700 RTP/AVP 96\na=rtpmap:96 H265/90000\n' > /tmp/beam.sdp
# keepalive-пінг у фоні:
( sudo ping -i 1 192.168.1.21 >/dev/null 2>&1 & )
# приймач: ffmpeg (RTP+RTCP, декод) → gst → HDMI:
"$FF" -protocol_whitelist file,udp,rtp -buffer_size 8388608 -reorder_queue_size 2000 \
   -fflags nobuffer -flags low_delay -i /tmp/beam.sdp -an -pix_fmt yuv420p -f rawvideo - 2>/dev/null | \
gst-launch-1.0 fdsrc ! rawvideoparse width=1280 height=1024 format=i420 framerate=25/1 \
   ! videoconvert ! <СИНК> sync=false
```
`<СИНК>` — той, що на цьому образі реально малює звичайні (не апаратні) кадри:
`xvimagesink` **або** `glimagesink` (див. §7 «Синки»). `rkximagesink` показує
**лише апаратно-декодовані** кадри (тому чорний для цього тракту), `ximagesink`
падає на XInput.

> ⚠ Впиши сюди точну команду/синк, якою відео нарешті пішло на екран — щоб
> документація була 1:1 з робочим станом.

### 4.3'. Альтернатива — апаратний декод (коли потік чистий)
Якщо радіоканал без втрат (напр. прямий Ethernet або сильний сигнал), працює
чистий gst-тракт із HW-декодом (менше CPU):
```sh
gst-launch-1.0 udpsrc port=5700 buffer-size=4194304 \
  caps="application/x-rtp,media=video,encoding-name=H265,payload=96,clock-rate=90000" \
  ! rtpjitterbuffer latency=100 ! rtph265depay ! h265parse ! mppvideodec \
  ! queue leaky=downstream max-size-buffers=3 ! rkximagesink sync=false
```
УВАГА: цей тракт `udpsrc` **не шле RTCP**, тож Beam його сам не заводить —
працює лише на вже теплій сесії. Для холодного старту потрібен ffmpeg (§4.3).

### 4.4. Керування службою
```sh
sudo systemctl status thermal-receiver
sudo systemctl restart thermal-receiver
sudo systemctl stop thermal-receiver
journalctl -u thermal-receiver -e
```

---

## 5. Sine.video Beam — ключові факти (виявлено на місці)

1. **Заводить потік не пінг і не пасивний слухач, а приймач, що шле RTCP.**
   `ffplay`/`ffmpeg` шлють RTCP receiver-report — і Beam стрімить. Голий
   `gst udpsrc` RTCP не шле → Beam на нього сам не починає слати.
2. **Keepalive-пінг обов'язковий і безперервний.** Поки приймач безперервно
   пінгує `192.168.1.21`, потік іде; пінг стих на кілька секунд — Beam глушить.
3. **Втрати радіо** («RTP: missed N packets») — реальні; від них апаратний декодер
   чорніє, софтовий (ffmpeg/ffplay) — терпить. Sine радить для поля: **max power
   + Dynamic power** на обох модемах.
4. Beam **не прозорий**: на борту тягне RTSP, на землі пушить RTP.
5. H.265 через Beam ходить (побоювання щодо H.264 не підтвердилось).

---

## 6. Повний перелік файлів у репозиторії

**Борт (RV1106):**
- `rv1106_sender/` — TCP-sender керівника (`src/main.cpp`, `Makefile*`, `README.mgr.md`);
- `rv1106_rtsp/` — RTSP-sender H.265 (`src/rtsp_sender.cpp`, `Makefile.prebuilt`, `README.md`);
- `board/etc/init.d/S99thermal` — автозапуск + watchdog; `board/README.md`.

**Земля (BPI-M7/RK3588):**
- `bpi/thermal_receiver.sh` — приймач + keepalive + самовідновлення;
- `bpi/thermal-receiver.service` — systemd-автозапуск;
- `bpi/thermal-receiver.desktop` — десктопний autostart (альтернатива);
- `bpi/README.md` — встановлення/діагностика.

**Документація:**
- `docs/INTERNAL_DOC.md` — цей файл (єдиний довідник);
- `docs/RADIO_LINK_SINE.md` — архітектура Beam, історія інтеграції, питання до Sine;
- `reproduce/BPI_M7_GROUND.md` — інструкція наземної станції;
- `RESULTS.md`, `README.md` — верхньорівневий опис.

**Допоміжне (розробка/діагностика):**
- `reproduce/01..05_*.sh` — збірка/деплой/запуск;
- `scripts/*.sh` — проби плати, захоплення/апскейл кадру, підключення Luckfox;
- `native/`, `fedora_receiver/` — ранні варіанти приймача на ноуті.

---

## 7. Порядок запуску (робочий процес)

1. **Борт:** увімкнути RV1106 (камера через живлений хаб + спільна земля). Плата
   сама піднімає RTSP (`S99thermal`, `thermal_mode=rtsp`) → `rtsp://192.168.1.10:554/stream=0`.
2. **Beam:** увімкнути обидва модулі (TX на борту тягне камеру, RX на землі).
3. **Земля:** увімкнути BPI-M7 з HDMI-монітором; кабель Beam RX → в порт BPI
   (end0 або end1); переконатись, що `192.168.1.2` на порту з кабелем.
4. Запустити приймач (§4.3) — керує ним ноут по WiFi (`ssh armsom@192.168.0.162`).

**Синки (важливо для §4.3):** на цьому образі відео звичайних кадрів малює
`xvimagesink`/`glimagesink`; `rkximagesink` — лише апаратні кадри; `ximagesink`
падає на XInput. Перевірка синка тестовою картинкою:
```sh
export DISPLAY=:0 XAUTHORITY=/home/armsom/.Xauthority
timeout 7 gst-launch-1.0 videotestsrc ! videoconvert ! xvimagesink
```

---

## 8. Часті проблеми й лік

| Симптом | Причина | Лік |
|---|---|---|
| BPI: `0 packets` / чорне | адреса `192.168.1.2` не на тому порту (кабель в іншому) | адресу на порт із `LOWER_UP` (§4.2) |
| BPI: чорне, хоч пакети йдуть | приймач без RTCP (голий gst udpsrc) | приймати через ffmpeg (§4.3) |
| Потік зникає за кілька секунд | немає безперервного keepalive-пінгу | тримати `ping -i 1 192.168.1.21` |
| Вікно є, але чорне | `rkximagesink` для не-апаратних кадрів | синк `xvimagesink`/`glimagesink` |
| «каша»/артефакти | втрати радіо + апаратний декод | софт-декод (ffmpeg) або підняти потужність Beam |
| `Destination Host Unreachable` | `192.168.1.2` на обох портах | лишити адресу лише на порту з кабелем |
| Борт: USB відвалюється/ребут | живлення/земля камери; високий fps | живлений хаб + спільна земля; fps=25 |
| `apt install ffmpeg` не ставиться | заморожений образ (конфлікт libav) | статичний ffmpeg (johnvansickle) |

---

## 9. Відкриті пункти / TODO

- **Автозапуск на RK «увімкнув → відео» повністю hands-free** ще потребує
  доопрацювання: служба має підняти адресу на правильному порту, статичний
  ffmpeg покласти в постійне місце (не `/tmp`, що чиститься на ребуті), і
  використати робочий синк (§4.3). Поточний `bpi/thermal_receiver.sh` — база;
  фіналізувати після фіксації точної команди приймача.
- **Апаратний декод замість софтового** — коли приберемо втрати радіо
  (потужність/антени/дальність Beam), повернути `mppvideodec` (менше CPU).
- **Постійний ffmpeg на BPI** — покласти бінар у `~/bin/` і посилатись на нього
  зі скрипта служби.
