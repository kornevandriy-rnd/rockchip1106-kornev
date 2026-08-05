# Відтворення з нуля: апаратний H.265-стрім Seek 640 з Luckfox Pico Ultra (RV1106)

Покрокова інструкція, щоб повторити робочий стрім **на іншому ноуті**.
Скрипти в цій теці автоматизують кожен крок.

```
Seek 640 (USB-UVC YUYV 640x512) → V4L2 → RGA (NV12 + апскейл 1280x1024)
  → RK_MPI_VENC (H.265, VEPU) → TCP :5000 → ffplay на ноуті
```

---

## 0. Передумови ЗАЛІЗА (без цього стрім рватиме)

- Плата **Luckfox Pico Ultra (RV1106)**, прошита Buildroot, доступна по SSH
  (`root@192.168.50.2`, пароль `luckfox`), USB у **host**-режимі (luckfox-config).
- Камера через **живлений USB-хаб** (порт плати сам струм не тягне).
- Живлення плати — **окремий 5V** на піни `5V`/`GND`, Type-C **порожній**.
- 🟥 **НАЙВАЖЛИВІШЕ: СПІЛЬНА ЗЕМЛЯ.** Мінуси (GND) блока плати, блока камери
  й хаба **з'єднані в одну точку**. Без цього USB-контролер плати ресетиться і
  весь хаб відвалюється кожні ~15с (`USB disconnect` у `dmesg`). Це був головний
  блокер. Перевірка мультиметром: між GND плати і GND камери має бути ~0 Ом.
- Короткий екранований USB-кабель плата↔хаб.

## 1. Передумови НОУТА (Fedora)

```sh
# інструменти
sudo dnf install -y git make v4l-utils openssh-clients
# ffmpeg з H.265-декодером (стоковий ffmpeg-free НЕ декодує HEVC):
sudo dnf install -y "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
sudo dnf swap ffmpeg-free ffmpeg --allowerasing
ffmpeg -hide_banner -decoders 2>/dev/null | grep -E ' hevc$'   # має показати hevc
```

## 2. Отримати код і SDK

```sh
# наш репозиторій (цей проєкт)
git clone https://github.com/kornevandriy-rnd/rockchip1106-kornev.git ~/rockchip1106-kornev
cd ~/rockchip1106-kornev && git checkout claude/luckfox-pico-linux-setup-wrz9u6

# Luckfox SDK — дає тулчейн + хедери RK_MPI/RGA (велике завантаження)
git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico
```

## 3. Зібрати (sender для плати + receiver для ноута)

```sh
cd ~/rockchip1106-kornev
BOARD_IP=192.168.50.2 LUCKFOX_SDK_DIR=$HOME/luckfox-pico ./reproduce/01_build.sh
```
Скрипт: додасть тулчейн у PATH, підтягне 3 бібліотеки з плати
(`librockit.so`, `librockchip_mpp.so`, `librga.so` → `rv1106_sender/prebuilt/`),
збере обидва бінари. **Важливо:** RGA-хедери беруться з **rv1106**-релізу SDK
(`media/rga/release_rga_rv1106_arm-rockchip830-linux-uclibcgnueabihf`), не rk3588.

## 4. Залити на плату і запустити

```sh
BOARD_IP=192.168.50.2 ./reproduce/02_deploy.sh        # scp бінара на /root плати
BOARD_IP=192.168.50.2 ./reproduce/03_run_board.sh     # запуск sender на платі (T1, блокує)
```
В іншому терміналі — приймач:
```sh
BOARD_IP=192.168.50.2 ./reproduce/04_run_receiver.sh  # ffplay
```

Має відкритись вікно з тепловізійним відео 1280×1024. У Терміналі 1 кожні 2с —
`Pipeline: XX.X fps, V4L2 dropped frames: N`.

---

## Чому саме так (три фікси, що коштували найбільше — див. `../RESULTS.md`)

1. **Спільна земля** (залізо) — прибрала ~15с USB-ресети.
2. **`send_all(encoded, len)` без `+u32Offset`** — не обрізати HEVC NAL.
3. **`RK_MPI_SYS_MmzFlushCache(pack.pMbBlk, RK_TRUE)`** перед читанням виходу
   VEPU — інакше CPU читає застарілий кеш → биті NAL.

## Типові проблеми

| Симптом | Причина / рішення |
|---|---|
| `USB disconnect` кожні ~15с | немає спільної землі → з'єднати GND (п.0) |
| `open V4L2 device: No such file` | нода камери «плаває» — скрипт шукає YUYV-ноду сам; перевір `lsusb \| grep 3474` |
| `Could not find ref / invalid NALU` | не той бінар (без кеш-фіксу) — перезбери з актуального коду |
| `im2d.hpp: No such file` при збірці | вказано rk3588-хедери; треба rv1106-реліз (скрипт це робить) |
| ffplay: `codec hevc not found` | стоковий ffmpeg-free → постав повний ffmpeg (п.1) |
| `cannot execute binary` | ARM-бінар запущено на ноуті; запускати ТІЛЬКИ на платі |
