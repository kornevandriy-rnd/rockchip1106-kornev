# Luckfox Pico Ultra (RV1106) — Термокамера + Upscale по USB

Робочий репозиторій проєкту: запуск **Luckfox Pico Ultra** (SoC **Rockchip RV1106**),
апаратний **upscale** відео (Rockchip RGA / ISP / NPU-RKNN) і вивід потоку
**термокамери Seek UAV 640** на дисплей.

> 📓 Хаб проєкту, журнал прогресу та інвентар заліза — у Notion:
> «🌡️ Luckfox Pico Ultra (RV1106) — Термокамера + Upscale по USB».
> Цей репозиторій тримає **скрипти та інструкції** для Linux-етапу (Fedora ↔ плата).

---

## 🧩 Залізо (коротко)

| | |
|---|---|
| SoC | Rockchip RV1106G3 (Cortex-A7 ~1.2 ГГц + RISC-V, NPU ~0.5 TOPS int8) |
| RAM / eMMC | 256 MB DDR3 · 8 GB eMMC (версія Ultra W) + microSD |
| ОС | Buildroot (`Luckfox_Pico_Ultra_EMMC_250607`), ядро Linux 5.10 (Rockchip BSP) |
| Камера | Seek UAV 640 (9 мм) — 640×512 @ 50 Гц, живлення **строго 5 В** |
| Доступ | USB-RNDIS → плата = `172.32.0.93` (root / luckfox) |

## ✅ Ключове рішення — Варіант A

Камера по **USB-UVC** (плата = host) → upscale → дисплей по **MIPI DSI**.
MIPI для камери відкинуто (несумісні роз'єми + протокол).

> ⚠️ **Один USB 2.0 OTG.** Зараз порт працює як **device** (RNDIS) — саме так
> ми заходимо по SSH на `172.32.0.93`. Для камери (Варіант A) порт треба
> перевести в **host**-режим, і тоді USB-мережа зникне. Тому **перед Кроком 2
> (камера)** треба підняти альтернативний доступ до плати:
> **Ethernet 100M** (пін-хедер/адаптер) або **UART-консоль** (CP2102/CH340, 3.3 В).
> Інакше після переходу в host втратимо і мережу, і доступ до плати.

---

## 🔌 Робочий процес (Fedora ↔ плата)

Плата фізично під'єднана до Fedora-хоста по USB. Скрипти запускаємо на Fedora;
вивід повертаємо в Notion-журнал.

```bash
git pull                      # підтягнути свіжі скрипти
chmod +x scripts/*.sh         # один раз

# 1. Під'єднатися до плати (авто-детект RNDIS + статична IP + ping)
./scripts/connect-luckfox.sh --ssh

# 2. Зібрати інвентар плати (uname, пам'ять, USB, /dev/video*)
ssh -o HostKeyAlgorithms=+ssh-rsa root@172.32.0.93 'sh -s' < scripts/board-probe.sh
```

## 📁 Скрипти

| Скрипт | Що робить | Де запускати |
|---|---|---|
| `scripts/connect-luckfox.sh` | Знаходить USB-RNDIS інтерфейс, вішає `172.32.0.100/16` через NetworkManager, пінгує плату, опційно SSH (`--ssh`). `--down` прибирає з'єднання. | Fedora-хост |
| `scripts/board-probe.sh` | Інвентар плати: система, CPU, пам'ять, накопичувач, режим USB-OTG, `/dev/video*`, USB-пристрої, наявність RGA/RKNN. | На платі (через ssh) |

### Інструменти на Fedora (знадобляться далі)
```bash
sudo dnf install -y v4l-utils android-tools nmap-ncat
```
`v4l-utils` — робота з камерою; `android-tools` — adb; `nmap-ncat` — діагностика.

---

## 🗺️ Статус і наступні кроки

- [x] Прошито Buildroot (eMMC), доступ по SSH через USB-RNDIS
- [ ] **Крок 1 (закриваємо):** логін із Fedora + `uname -a` → підтвердити систему
- [ ] Крок 2: підняти Ethernet/UART доступ, під'єднати Seek 640 по USB-UVC, знайти `/dev/video*`
- [ ] Крок 3: сирий кадр з камери (формат / роздільність / fps)
- [ ] Крок 4: базовий upscale через RGA (2D scaling)
- [ ] (Опц.) AI-upscale через NPU / RKNN
- [ ] Вивід на дисплей по MIPI DSI, заміри латентності/FPS

## 🔗 Ресурси
- Luckfox Pico Ultra Wiki — <https://wiki.luckfox.com/Luckfox-Pico/Luckfox-Pico-Ultra/>
- Luckfox Pico SDK — <https://github.com/LuckfoxTECH/luckfox-pico>
- Rockchip RGA — <https://github.com/airockchip/librga>
- RKNN Toolkit 2 — <https://github.com/airockchip/rknn-toolkit2>
