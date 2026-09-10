# RV1106 (борт) — камера як RTSP-джерело для Beam

Усе, щоб з нуля відтворити бортову плату: тепловізор Seek → RV1106 → апаратний
H.265 → RTSP-сервер, який тягне Beam TX.

```
Seek 640 (USB-UVC, YUYV 640×512) → RV1106: V4L2 → RGA (апскейл 1280×1024, NV12)
   → VEPU (H.265 CBR) → RTSP :554/stream=0 → Beam TX
```

## Вміст папки
| Файл | Що це |
|---|---|
| `src/rtsp_sender.cpp` | **головний** — RTSP-сервер H.265 (робочий варіант для Beam) |
| `Makefile.rtsp.prebuilt` | збірка `rtsp_sender.cpp` → бінар `rv1106_rtsp` |
| `src/tcp_sender_main.cpp` | запасний — той самий конвеєр, але вихід TCP :5000 (прямий тракт плата↔плата) |
| `Makefile.tcp.prebuilt` | збірка TCP-варіанта → бінар `rv1106_sender` |
| `init.d/S99thermal` | автозапуск + watchdog камери на платі |

---

## 0. Що потрібно (один раз на ноуті-збиральнику)

### SDK Luckfox (тулчейн + хедери)
```sh
git clone https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico
cd ~/luckfox-pico && ./build.sh lunch   # обрати RV1106 / Luckfox Pico Ultra; можна не збирати образ цілком
```
Головне зі SDK для нас:
- тулчейн: `~/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/`
- хедери RK_MPI: `~/luckfox-pico/media/rockit/rockit/mpi/sdk/include`
- хедери RGA: `~/luckfox-pico/media/rga/release_rga_rv1106_.../include/rga`
- RTSP-сервер: `~/luckfox-pico/media/common_algorithm/common_algorithm/misc` (`include/rtsp_demo.h` + `lib/.../librtsp.a`)

### 3 бібліотеки з САМОЇ плати (у `prebuilt/`)
```sh
cd <ця_папка> && mkdir -p prebuilt
for l in librockit.so librockchip_mpp.so librga.so; do scp root@192.168.50.2:/oem/usr/lib/$l prebuilt/; done
```

---

## 1. Збірка (крос-компіляція на ноуті)
```sh
export PATH=$HOME/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH

# RTSP-варіант (для Beam) — головний:
make -f Makefile.rtsp.prebuilt LUCKFOX_SDK_DIR=$HOME/luckfox-pico      # → бінар rv1106_rtsp

# TCP-варіант (прямий тракт, за потреби):
make -f Makefile.tcp.prebuilt  LUCKFOX_SDK_DIR=$HOME/luckfox-pico      # → бінар rv1106_sender
```
Makefile сам перевіряє наявність компілятора, хедерів і `librtsp.a`; якщо чогось
нема — скаже, чого саме.

---

## 2. Заливка на плату (BusyBox: без scp/SFTP — тому scp бінара окремо)
Доступ: `ssh root@192.168.50.2`.
```sh
# бінарники:
scp rv1106_rtsp   root@192.168.50.2:/root/
scp rv1106_sender root@192.168.50.2:/root/     # якщо збирав TCP-варіант

# автозапуск (BusyBox scp бінара працює; текст init-скрипта — через heredoc):
ssh root@192.168.50.2 'cat > /etc/init.d/S99thermal' < init.d/S99thermal
ssh root@192.168.50.2 'chmod +x /etc/init.d/S99thermal'

# режим виходу — RTSP (для Beam):
ssh root@192.168.50.2 'echo rtsp > /root/thermal_mode'
```

### IP плати в мережі Beam (персистентно)
Плата має бути на `192.168.1.10` (камерна підмережа Beam), поряд зі службовим
`192.168.50.2`. Додати в `/etc/init.d/S99eth0static` (start-гілка) рядок:
```sh
ip addr add 192.168.1.10/24 dev eth0 2>/dev/null
```
(і симетрично `ip addr del …` у stop-гілку). Перевірка: `ip addr show eth0`
має показати і `192.168.50.2`, і `192.168.1.10`.

---

## 3. Запуск і перевірка
```sh
ssh root@192.168.50.2 '/etc/init.d/S99thermal restart; sleep 3; /etc/init.d/S99thermal status'
```
Має бути: режим `rtsp`, `rv1106_rtsp ПРАЦЮЄ`, камера `devnum=…`.

**Перевірити RTSP БЕЗ Beam** (прямий кабель/WiFi до плати, з ноута):
```sh
ffplay -rtsp_transport tcp rtsp://192.168.1.10:554/stream=0
```
Побачив тепловізор → камера-RTSP працює.

**У меню Beam TX:** Cam IP = `192.168.1.10`, потік `rtsp://192.168.1.10:554/stream=0`.

---

## 4. Важливі нюанси (щоб «без проблем»)
- **Камера:** живлений USB-хаб + **спільна земля** з платою (інакше ребут-луп/відвал хаба).
- **fps=25** у S99thermal навмисне: менший USB-трафік прибирає EOVERFLOW (`-75`)
  і «disabled by hub», що валили USB під тривалим стрімом.
- `rkipc` (штатний застосунок) тримає камеру й порт 554 — S99thermal його гасить
  (`RkLunch-stop.sh` + `killall`).
- Нода камери `/dev/videoN` «плаває» — S99thermal сам обирає USB+YUYV-ноду.
- `LD_LIBRARY_PATH=/oem/usr/lib` обов'язковий для бінара (там librockit тощо).

## 5. Параметри бінара (за потреби)
`--device`, `--port`, `--path` (деф. `/stream=0`), `--fps`, `--bitrate` (кбіт/с),
`--gop`, `--output-width/-height` (деф. 1280×1024). Для радіо з втратами — менший
GOP (`--gop 30`, частіші IDR → швидше відновлення).
