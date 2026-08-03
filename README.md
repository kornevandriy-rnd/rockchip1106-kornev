# Luckfox Pico Ultra (RV1106) — Термокамера + Upscale + Дисплей

Робочий репозиторій проєкту: запуск **Luckfox Pico Ultra** (SoC **Rockchip RV1106**),
апаратний **upscale** відео (Rockchip **RGA** / ISP / NPU-RKNN) і вивід потоку
**термокамери Seek UAV 640** на локальний **RGB-дисплей**.

> 📓 Хаб проєкту, журнал прогресу та інвентар заліза — у Notion:
> «🌡️ Luckfox Pico Ultra (RV1106) — Термокамера + Upscale по USB».
> Цей репозиторій тримає **скрипти та інструкції** для Linux-етапу (Fedora ↔ плата).

---

## 🧩 Залізо (коротко)

| | |
|---|---|
| SoC | Rockchip RV1106G3 (Cortex-A7 ~1.2 ГГц + RISC-V, NPU ~0.5 TOPS int8, RGA2) |
| RAM / eMMC | 256 MB DDR3 · 8 GB eMMC (версія Ultra W) + microSD |
| ОС | Buildroot (`Luckfox_Pico_Ultra_EMMC_250607`), ядро Linux 5.10 (Rockchip BSP) |
| Камера | Seek UAV 640 (9 мм) — 640×512, **живлення строго 5 В** |
| USB камери | UVC `3474:43e2` (UVC 1.10) + CP210x-керування `10c4:ea60`, через USB-хаб `1a86:8091` |
| Дисплей | **RGB (паралельний, НЕ MIPI DSI!)**, офіційна панель Luckfox 720×720 або 480×480 |
| **Доступ (основний)** | **Ethernet → `192.168.50.2`** (root / luckfox) |
| Доступ (резерв) | USB-RNDIS → `172.32.0.93` — **лише коли USB у device-режимі** (без камери) |

> ⚠️ Ethernet — головний канал доступу. Він **обов'язковий**, бо для камери
> єдиний USB переводиться в **host** і USB-RNDIS (`172.32.0.93`) зникає.

## ✅ Архітектура — Варіант A (працює)

```
Камера Seek 640 (USB-UVC, YUYV 640×512)
   → RGA (YUV→RGB + масштаб до розміру панелі)
   → /dev/fb0 (RGB-дисплей)
```

- Камера по **USB-UVC** — плата в **host**-режимі. ✅ Підтверджено: живий тепловий кадр.
- **MIPI для камери відкинуто** (несумісні роз'єми/протокол — Seek віддає USB/CVBS).
- Дисплей — **RGB-паралельний** (у Pico Ultra НЕ MIPI DSI, попри перше припущення).

---

## 🔗 Повний робочий ланцюжок камери (важливо!)

Це неочевидно і зайняло багато налагодження. Порядок і всі умови:

1. **Живлення плати — ОКРЕМЕ** (мережевий блок 5 В на піни `5V`/`GND`).
   Бо USB-C звільняється під камеру й більше не живить плату.
2. **USB-C (`USB & Power`) — ПОРОЖНІЙ.** На платі стоїть **switch-чіп**:
   - Type-C **запитаний** → єдиний USB йде в **Type-C**;
   - Type-C **знеструмлений** → USB перемикається на **USB-A** (`USB`, порт біля Ethernet).
   Тобто камеру бачить лише USB-A, і лише коли на Type-C нема живлення.
3. **USB у host-режимі:** `luckfox-config` → *Advanced Options* → *USB* → **host** → `reboot`.
   Перевірка: `cat /sys/class/extcon/*/state` → `USB-HOST=1`, `USB_VBUS_EN=1`;
   `/sys/class/udc/` — порожній.
4. **ЖИВЛЕНИЙ USB-хаб** між USB-A і камерою — **обов'язково**.
   Порт плати не витягує струм камери: без хаба цикл `can't set config #1, error -71`
   (brown-out на SET_CONFIG). З живленим хабом камера піднімається стабільно.
5. Камера з'являється як `/dev/videoNN` (**номер плаває** після реенумерацій!).
   Формат: **YUYV 4:2:2, 640×512 @ 60/30 fps**.

### ⚠️ Відомі «граблі»
- **`error -75` / `disabled by hub (EMI?)`** при стрімі — сигнальні зриви на лінії
  плата↔хаб. Одиночні кадри ловляться, але для плавного **відео** потрібен
  **короткий екранований** USB-кабель до хаба (подалі від блока живлення/Ethernet).
- **Номер `/dev/videoN` не стабільний** — шукати ноду динамічно (див. `capture-frame.sh`).
- На платі **немає gcc** — програми на librga треба **крос-компілювати** через SDK.

---

## 🖥️ Дисплей (RGB) — стан і застереження

- Плата — **RGB666 паралельний** інтерфейс, 40-pin FPC роз'єм `LCD`. DRM активний:
  `/dev/fb0`, `/dev/dri/card0`. Типова панель — 720×720 (за замовч.) або 480×480.
- Кастомний таймінг задається **без правки device tree**, через `luckfox-config`:
  *Advanced Options* → *RGB* → **enable** → форма **Enter RGB Parameters**
  (clock, полярності hsync/vsync/de/pclk, hactive/vactive, back/front-porch, sync-len).
  Приклад для 480×272: clock `9000000`, hact `480`, vact `272`, HBP/HFP/VBP/VFP `2`,
  Hsync-len `41`, Vsync-len `10`, полярності `0/0/1/0`.
- ❗ **Обмеження:** `luckfox-config` не приймає **hactive > 640**.
- 🔴 **ВАЖЛИВО про сумісність:** розкладка 40 пінів у роз'ємі Pico Ultra **НЕ така**,
  як у generic 40-pin LCD. **Generic-дисплей напряму не підходить** (потрібен адаптер;
  Luckfox прямо попереджає про ризик спалити залізо через КЗ при невідповідності пінів).
  Перевірено: generic `HY0430IPS04-40` (4.3″ 480×272, NV3047) — плата коректно гнала
  480×272@60 на `fb0`, але екран лишався чорним (розкладка не збіглася).
  **Рішення: брати ОФІЦІЙНИЙ дисплей Luckfox під Pico Ultra** (720×720, RGB, з тачем
  Goodix — він уже в прошивці) → plug-and-play.

---

## ⚡ RGA — апаратний upscale (перевірено)

- `/dev/rga` + `/oem/usr/lib/librga.so` (треба `LD_LIBRARY_PATH=/oem/usr/lib`).
- Тест-утиліта: `/oem/usr/bin/rgaImDemo` (демо з фіксованими 720p-картинками).
- Можливості (`rgaImDemo --querystring all`): **RGA2_Enhance**, вхід до 8192², вихід до 4096²,
  **scale 0.0625..16×**, вхід **YUYV422**, вихід **RGBA/RGB888/RGB565**, апаратний **CSC (YUV→RGB)**.
- Швидкість: ресайз 720p→1080p ≈ **4.6 мс** → real-time 60 fps із запасом, майже без CPU.
- Для ресайзу **власного** кадру потрібна маленька програма на im2d/librga
  (`imresize`/`imcvtcolor`) — **крос-компільована** (на платі нема gcc).

---

## 🔌 Робочий процес (Fedora ↔ плата)

Доступ — по **Ethernet**. Команди зручно ганяти одним рядком (не залежить від того,
у якому терміналі ти сидиш):

```bash
# перевірка/інвентар
ssh root@192.168.50.2 'uname -a; lsusb; ls /dev/video*'

# захопити кадр із камери (авто-пошук ноди + retry) і забрати PNG
ssh root@192.168.50.2 'sh -s' < scripts/capture-frame.sh   # створить /root/frame.png на платі
scp root@192.168.50.2:/root/frame.png ~/                    # забрати на Fedora

# апскейл кадру під дисплей
ssh root@192.168.50.2 'sh -s -- /root/frame.yuyv /root/up_720.png 720' < scripts/upscale-frame.sh
```

### Інструменти на Fedora
```bash
sudo dnf install -y v4l-utils android-tools nmap-ncat ffmpeg
```

## 📁 Скрипти

| Скрипт | Що робить | Де запускати |
|---|---|---|
| `scripts/connect-luckfox.sh` | Знаходить USB-RNDIS інтерфейс, вішає IP, пінгує, опційно SSH. *(актуально лише в device-режимі USB)* | Fedora-хост |
| `scripts/board-probe.sh` | Інвентар плати: система, CPU, пам'ять, USB-OTG, `/dev/video*`, RGA/RKNN. | На платі (ssh) |
| `scripts/check-uvc-readiness.sh` | Перевірка готовності до камери: `uvcvideo`, host/gadget, v4l2, RGA/RKNN. | На платі (ssh) |
| `scripts/capture-frame.sh` | Захват 1 кадру з UVC-камери (авто-пошук `/dev/videoN` + retry) → PNG (normalize). | На платі (ssh) |
| `scripts/upscale-frame.sh` | Софтовий апскейл кадру 640×512 → 720×720 (letterbox, ffmpeg). | На платі (ssh) |

---

## 🗺️ Статус і наступні кроки

- [x] Прошито Buildroot (eMMC), доступ по SSH
- [x] **Крок 1:** підтверджено систему (RV1106, Linux 5.10, Luckfox Pico Ultra)
- [x] **Крок 2:** Ethernet-доступ + USB→host + камера по USB-UVC → `/dev/video*` (YUYV 640×512)
- [x] **Крок 3:** сирий тепловий кадр (підтверджено — рука в теплі на холодному фоні)
- [x] Перевірено можливості RGA (YUYV→RGB + масштаб, ~4.6 мс)
- [ ] **Дисплей:** замовити офіційний Luckfox RGB 720×720 (generic не підійшов)
- [ ] **Стабільність відео:** короткий екранований кабель плата↔хаб (проти EMI `-75`)
- [ ] **Крок 4 (MVP):** real-time в'юер `ffmpeg -f v4l2 … -f fbdev /dev/fb0` (софт, без компіляції)
- [ ] **Крок 4 (оптимізований):** крос-компільований `V4L2 → RGA → fb0` на librga (60 fps, low-CPU)
- [ ] (Опц.) NUC-калібрування шторкою (прибрати fixed-pattern noise)
- [ ] (Опц.) AI-upscale через NPU / RKNN
- [ ] Заміри латентності/FPS

## 🔗 Ресурси
- Luckfox Pico Ultra Wiki — <https://wiki.luckfox.com/Luckfox-Pico/Luckfox-Pico-Ultra/>
- Luckfox Pico Ultra — RGB screen — <https://wiki.luckfox.com/Luckfox-Pico-Ultra/RGB-Screen/>
- Luckfox Pico SDK — <https://github.com/LuckfoxTECH/luckfox-pico>
- Rockchip RGA (librga / im2d) — <https://github.com/airockchip/librga>
- RKNN Toolkit 2 — <https://github.com/airockchip/rknn-toolkit2>
