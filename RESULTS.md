# Результат: апаратний стрім тепловізора Seek 640 з Luckfox Pico Ultra (RV1106)

**Статус: ПРАЦЮЄ ✅** — живий апаратний H.265-стрім тепловізора на ноут по Ethernet.

## Що зроблено

Наскрізь на залізі RV1106, **без ffmpeg на платі**:

```
Seek 640 (USB-UVC, YUYV 640x512)
  → V4L2 (захват)
  → RGA (YUYV → NV12 + апаратний апскейл 640x512 → 1280x1024)
  → RK_MPI_VENC / VEPU (апаратний H.265, CBR)
  → TCP :5000 (raw Annex-B HEVC)
  → Fedora: fedora_receiver → ffplay (nvdec/ЦП декод)
```

- **Плата:** `rv1106_sender` (код керівника, `rv1106_sender/`)
- **Ноут:** `fedora_receiver` (запускає ffplay напряму)

## Три ключові виправлення (те, що коштувало найбільше часу)

Стрім не працював через **три незалежні проблеми**, кожну з яких довелося знайти окремо:

1. **Спільна земля (ЗАЛІЗО — головний блокер).**
   Плата, камера й USB-хаб живились окремо, а їхні мінуси (GND) не були
   з'єднані. Через «плаваючу» землю USB-контролер плати ресетився і **весь хаб
   з камерою відвалювався кожні ~15 секунд** (`USB disconnect` у dmesg).
   Рішення: **спільна земля** для плати/камери/хаба + окреме живлення плати 5V +
   живлений хаб + короткий екранований кабель. Після цього USB стабільний.

2. **Подвійний зсув `u32Offset` (КОД).**
   `Handle2VirAddr(pMbBlk)` уже вказує на початок даних пака; додавання
   `u32Offset` вдруге обрізало кожен HEVC NAL-юніт → потік розсипався.
   Фікс: `send_all(encoded, pack.u32Len)` без `+ u32Offset`.

3. **Когерентність кешу (КОД — фінальний фікс чистої картинки).**
   VEPU пише закодований H.265 у DMA-буфер, а Cortex-A7 читав його через
   кешовану мапу і брав **застарілі рядки кешу** → періодично биті NAL
   (`Could not find ref / invalid NALU / PPS out of range`).
   Фікс: `RK_MPI_SYS_MmzFlushCache(pack.pMbBlk, RK_TRUE)` (invalidate) перед
   читанням закодованого буфера.

## Складання (крос-компіляція на Fedora)

На платі компілятора немає — збираємо тулчейном Luckfox SDK.

```sh
# 1. тулчейн у PATH
export PATH=$HOME/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH

# 2. бібліотеки з плати (для лінковки) у rv1106_sender/prebuilt/
mkdir -p rv1106_sender/prebuilt
scp root@192.168.50.2:/oem/usr/lib/librockit.so       rv1106_sender/prebuilt/
scp root@192.168.50.2:/oem/usr/lib/librockchip_mpp.so rv1106_sender/prebuilt/
scp root@192.168.50.2:/oem/usr/lib/librga.so          rv1106_sender/prebuilt/

# 3. sender — ВАЖЛИВО: RGA-хедери з RV1106-релізу (не rk3588!):
make -C rv1106_sender -f Makefile.prebuilt LUCKFOX_SDK_DIR=$HOME/luckfox-pico \
  RGA_INC=$HOME/luckfox-pico/media/rga/release_rga_rv1106_arm-rockchip830-linux-uclibcgnueabihf/include/rga

# 4. приймач
make -C fedora_receiver

# 5. бінар на плату
scp rv1106_sender/rv1106_sender root@192.168.50.2:/root/
```

> `media/out` у свіжому клоні SDK порожній (media не збирали), тому оригінальний
> Makefile керівника (через `media/Makefile.param`) хедерів не знаходить.
> Тому збираємо `Makefile.prebuilt`, вказавши RGA-реліз **rv1106** напряму
> (rk3588 — чужа архітектура й старий стиль хедерів, з ним код не компілюється).

## Запуск

**Плата** (нода `/dev/videoN` «плаває» — знайти YUYV-ноду):
```sh
ssh root@192.168.50.2 'export LD_LIBRARY_PATH=/oem/usr/lib; \
  pkill -9 rkipc 2>/dev/null; pkill -9 ispserver 2>/dev/null; \
  CAM=$(for v in /dev/video*; do v4l2-ctl -d "$v" --list-formats 2>/dev/null | grep -q YUYV && { echo "$v"; break; }; done); \
  /root/rv1106_sender --device "$CAM" --port 5000'
```
(Дефолти: 640x512@60 → 1280x1024, H.265 CBR 8000 kbit/s, GOP 60.)

**Ноут:**
```sh
./fedora_receiver/fedora_receiver 192.168.50.2 5000
```

## Виміряні показники

_(заповнюється з реального прогону — рядок `Pipeline: … fps …` з плати)_

| Параметр | Значення |
|---|---|
| Вхід камери | YUYV 640×512 |
| Вихід кодування | H.265 1280×1024 (RGA-апскейл ×2) |
| Цільовий бітрейт | 8000 kbit/s CBR |
| Реальний FPS (плата) | _TBD_ |
| V4L2 dropped | _TBD_ |
| Розмір 10с-кліпу | _TBD_ |

## Наступні кроки (беклог)

- [ ] Тюнінг fps/бітрейту під стабільні 30/60 без дропів
- [ ] Контраст термалки (AGC) — сира термалка малоконтрастна
- [ ] Офіційний дисплей Luckfox RGB 720×720 (вивід локально на плату)
- [ ] Латентність end-to-end
