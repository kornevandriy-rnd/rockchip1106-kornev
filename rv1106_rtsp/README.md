# rv1106_rtsp — RTSP-камера з RV1106 для Sine.video Beam

Beam приймає відео **по RTP/RTSP** (відповідь Sine, див. `../docs/RADIO_LINK_SINE.md`),
а не тунелює наш сирий TCP-потік. Тож RV1106 має виглядати як **RTSP-камера**.

Цей компонент — той самий перевірений конвеєр, що й `../rv1106_sender` керівника
(V4L2 YUYV → RGA NV12+апскейл 1280×1024 → RK_MPI_VENC H.265 CBR), **але вихід —
RTSP-сервер** (Rockchip `librtsp`). Файл керівника не змінюється; це окрема збірка.

```
Seek 640 → RV1106: V4L2 → RGA → H.265(VEPU) → RTSP :554/live/0
   → Beam Tx (тягне rtsp://192.168.50.2:554/live/0) ))) радіо ))) Beam Rx → монітор
```

URL за замовчуванням: **`rtsp://<IP-плати>:554/live/0`** (IP плати = `192.168.50.2`).

---

## Збірка (на ноуті)

Потрібен Luckfox SDK (тулчейн + хедери) і 3 бібліотеки з плати (ті самі, що для
`rv1106_sender`). Найпростіше — переюзати вже наявні:

```sh
cd ~/rockchip1106-kornev/rv1106_rtsp
mkdir -p prebuilt && cp ../rv1106_sender/prebuilt/*.so prebuilt/ 2>/dev/null || {
  # якщо ще не тягнув — з плати:
  for l in librockit.so librockchip_mpp.so librga.so; do scp root@192.168.50.2:/oem/usr/lib/$l prebuilt/; done
}
export PATH=$HOME/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
make -f Makefile.prebuilt LUCKFOX_SDK_DIR=$HOME/luckfox-pico
```
Отримаєш ARM-бінар `rv1106_rtsp`. (RTSP-сервер `librtsp.a` береться зі SDK:
`media/common_algorithm/common_algorithm/misc`.)

## Заливка і запуск на платі

```sh
ssh root@192.168.50.2 'pkill -9 rv1106_rtsp 2>/dev/null; pkill -9 rkipc 2>/dev/null'
scp rv1106_rtsp root@192.168.50.2:/root/
ssh root@192.168.50.2 'export LD_LIBRARY_PATH=/oem/usr/lib; \
  pkill -9 rkipc 2>/dev/null; pkill -9 ispserver 2>/dev/null; pkill -9 rv1106_sender 2>/dev/null; \
  pkill -9 rv1106_rtsp 2>/dev/null; sleep 1; \
  /root/rv1106_rtsp --device /dev/video0 --port 554 --path /live/0'
```
> `rkipc` треба прибити — він теж тримає порт 554.

Має вивести `RTSP: rtsp://<IP-плати>:554/live/0` і далі `Pipeline: … fps`.

## Крок 1 — перевірити RTSP БЕЗ Beam (важливо)

Спершу переконайся, що сам RTSP живий — з ноута/BPI (прямий кабель або WiFi до плати):
```sh
ffplay -rtsp_transport tcp rtsp://192.168.50.2:554/live/0
# або gstreamer:
gst-launch-1.0 rtspsrc location=rtsp://192.168.50.2:554/live/0 latency=100 ! rtph265depay ! h265parse ! avdec_h265 ! autovideosink
```
Побачив тепловізор → RTSP-камера працює. Тепер можна віддавати її Beam.

## Крок 2 — під'єднати до Beam

1. RV1106 (eth0) → Beam Tx; Beam Rx → монітор/земля (як велить Sine).
2. У меню Beam у полі джерела камери вписати: **`rtsp://192.168.50.2:554/live/0`**
   (переконайся, що Beam-Tx на камерному боці бачить IP `192.168.50.2`; якщо в
   Beam інша камерна підмережа — або зміни IP плати під неї, або вкажи в URL
   потрібний IP).
3. Польові тести: на обох модемах — **макс. потужність** + **Dynamic power** (порада Sine).

## Параметри (за потреби)

`--bitrate` (кбіт/с), `--fps`, `--gop`, `--output-width/-height`, `--path`, `--port`.
Для радіо з втратами варто **менший GOP** (частіші IDR → швидше відновлення), напр.
`--gop 30`, і бітрейт під заміряну смугу Beam.

## Примітки

- RTSP-сервер стрімить **безперервно** (як IP-камера); Beam/ffplay під'єднуються будь-коли.
- Кодек — H.265; `rtsp_set_video` отримує VPS+SPS+PPS з першого IDR автоматично.
- Якщо Beam вимагає саме **RTP-push** (а не тягне RTSP) — скажи, додам режим
  UDP/RTP на IP:порт Beam (це інша гілка, але той самий конвеєр).
