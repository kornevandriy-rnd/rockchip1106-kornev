# Телеметрія польотника → HUD у відео (RV1106)

Радіо (Sine mini/Beam) возить лише відео, тому телеметрія накладається на картинку **на борту** до кодування.

```
Польотник ──UART/MAVLink──▶ RV1106: mav_state (парсер) ──▶ osd (малює HUD у YUYV 640×512)
Seek 640 (YUYV) ──memcpy──▶ [HUD] ──▶ RGA ×2 ──▶ H.265 ──▶ RTSP ──▶ радіо
```

## Файли (`rv1106_rtsp/`)
| Файл | Що |
|---|---|
| `src/mav_state.{h,cpp}` | MAVLink v1/v2 без залежностей: розбір з пересинхронізацією, CRC, читач UART/UDP у потоці, запит потоків у польотника (heartbeat + `SET_MESSAGE_INTERVAL`) |
| `src/osd.{h,cpp}` | HUD: режим/arm, GPS, батарея, висота, швидкість, курс, координати, відстань до дому, штучний горизонт; стан «NO FC LINK» замість застарілих даних |
| `src/font8x16.h` | шрифт Terminus 8×16 (OFL), згенерований `tools/gen_font.py` |
| `test/` | `test_mav.py` (pymavlink кодує, C++ розбирає), `osd_demo.cpp` (малює HUD у файл) |

## Параметри `rv1106_rtsp`
`--mav uart:/dev/ttyS3:115200 | udp:14550 | off` (за замовчуванням `off`), `--osd 0|1` (1), `--mav-request 0|1` (1; 0 = лише слухати).
`udp:PORT` — для налагодження з симулятором (запити польотнику не шлються).

## Повідомлення, які читаються
HEARTBEAT, SYS_STATUS, SYSTEM_TIME, GPS_RAW_INT, ATTITUDE, GLOBAL_POSITION_INT, VFR_HUD, HOME_POSITION. Режим польоту розшифровується для ArduPilot (copter/plane) і PX4.

## Перевірка
```sh
# розбір MAVLink (потрібен pymavlink, напр. з ~/seek-ground-station/.venv)
cd rv1106_rtsp && ~/seek-ground-station/.venv/bin/python -m pytest test -q
# вигляд HUD на хості: ok | nolink | nofix | lowbatt
g++ -std=gnu++11 -o /tmp/osd_demo test/osd_demo.cpp src/osd.cpp src/mav_state.cpp -lpthread && /tmp/osd_demo ok /tmp/f.yuyv
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 640x512 -i /tmp/f.yuyv -vf scale=1280:1024 /tmp/f.png
# збірка під плату
make -f Makefile.prebuilt LUCKFOX_SDK_DIR=$HOME/luckfox-pico
```

## Обмеження
- Радіо односпрямоване: команд з землі в польотник через mini нема (лише перегляд).
- HUD «вшитий» у кадр; вимкнути на землі не можна (`--osd 0` на борту).
- Статус на борту: зібрано й перевірено на хості; **на живій платі з камерою ще не запускалось** (потрібні: з'єднання з RV1106, UART до польотника або симулятор по UDP).


## Стан на 23.10.2026 (зафіксовано, працює на живій платі)
**Польотник:** Matek H743-Slim V3 (ArduPilot, `BRD_ALT_CONFIG=1`). Підписи TX1/RX1 на платі = UART1 = **SERIAL2**: `SERIAL2_PROTOCOL=2`, `SERIAL2_BAUD=230` (230400). Початкові параметри: `~/fc_backup/matekh743_params_ORIGINAL_before_changes.param`. Лічильники портів: MAVFTP `@SYS/uarts.txt`.

**Підключення до RV1106:** TX1(FC) → GPIO1_A1 (UART3_M0_RX), RX1(FC) → GPIO1_A0 (UART3_M0_TX), GND ↔ GND; на борту `/dev/ttyS3` @ 230400. UART вмикається `luckfox-config` (динамічний overlay, `/etc/luckfox.cfg`: `UART3_M0_STATUS=1`; рядок `UART3_M1_STATUS` не залишати, інакше другий overlay вимикає перший).

**Що показує HUD (вибір оператора):** режим + ARMED/DISARMED (верх-ліворуч), BAT у вольтах (верх-праворуч; відсотків польотник не віддає, датчика струму немає), ALT (відносна висота), SPD (наземна швидкість), HDG (курс), горизонт. Вимкнено за замовчуванням (`Osd::extra=false`): GPS, газ, VS, координати, відстань до дому, струм.
Немає зв'язку з польотником → «NO FC LINK» замість застарілих цифр (перевірено на хості, живе не перевірено).

**Фільтр джерел:** на лінії є пересланий трафік компаньйона (sys 100 / comp 191: HEARTBEAT типу GCS ~5 Гц і GLOBAL_VISION_POSITION_ESTIMATE ~20 Гц); код приймає лише повідомлення від системи польотника (`sys == sysid`), а `fc_sys_` береться лише з його heartbeat.

**Розгортання на борту:** `/root/rv1106_rtsp` (нова), резерв `/root/rv1106_rtsp.nohud`; у `/etc/init.d/S99thermal` змінна `MAV_ARGS="--mav uart:/dev/ttyS3:230400"` (порожній рядок вимикає HUD), резерв скрипта `/root/S99thermal.nohud`. Збірка: `make -f Makefile.prebuilt LUCKFOX_SDK_DIR=$HOME/luckfox-pico`.

**Виміряно:** SoC 85–86 °C з HUD (79–82 без), fps 58,5; HUD на землі через mini → RK: видно, станція 49 fps. Відомий ризик: поріг 85 °C; потрібен охоронець перегріву.
