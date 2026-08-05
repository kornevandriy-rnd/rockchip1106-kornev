# thermal_stream — апаратний відеострім тепловізора на RV1106

Нативний застосунок для **Luckfox Pico Ultra (Rockchip RV1106)**: бере кадри з
USB-UVC тепловізора Seek 640, робить **усе на залізі** й віддає H.264/H.265 у
мережу. **ffmpeg на платі не використовується** — так вимагає архітектура
(на RV1106 немає V4L2-M2M енкодера, апаратний кодек доступний тільки через MPP/rockit).

```
/dev/video21 (YUYV 640x512) ──V4L2──▶ RGA (YUYV→NV12, +масштаб) ──▶ RK_MPI_VENC (VEPU) ──▶ TCP :8080 ──▶ плеєр на ноуті
```

## Чому саме так (перевірено експериментами)

| Спроба | Результат |
|---|---|
| `ffmpeg -c:v hevc_rkmpp` | у Luckfox-ffmpeg немає rkmpp-енкодера |
| `ffmpeg -c:v h264_v4l2m2m` | `Could not find a valid device` — на RV1106 нема V4L2-M2M енкодер-ноди |
| `mpi_enc_test -f 8` (YUYV напряму) | рідер не читає YUYV → порожні кадри |
| `mpi_enc_test -f 0` (NV12) | ✅ апаратний H.264 працює (доведено: 1.1 Мбіт/с з рухом) |

Звідси висновок: камера дає **тільки YUYV** → потрібна конверсія YUYV→NV12
(робить **RGA**, апаратно), а кодує **VEPU** через `RK_MPI_VENC`.

## Збірка (на ноуті, крос-компіляція)

На платі gcc немає — збираємо тулчейном Luckfox SDK.

### 1. Тулчейн (Luckfox SDK)
```sh
git clone --depth 1 https://github.com/LuckfoxTECH/luckfox-pico.git ~/luckfox-pico
export LUCKFOX_SDK_DIR=~/luckfox-pico
ls $LUCKFOX_SDK_DIR/tools/linux/toolchain/       # має бути arm-rockchip830-linux-uclibcgnueabihf
```

### 2. Знайти хедери RK_MPI і бібліотеки
```sh
# хедери rockit (rk_mpi_venc.h …)
find $LUCKFOX_SDK_DIR -name rk_mpi_venc.h -printf '%h\n' | head -1
# librockit.so і librga.so — якщо в SDK їх нема, беремо з ПЛАТИ:
mkdir -p native/prebuilt
scp root@192.168.50.2:/oem/usr/lib/librockit.so native/prebuilt/
scp root@192.168.50.2:/oem/usr/lib/librga.so    native/prebuilt/
```

### 3. Зібрати
```sh
cd native
make RKMPI_INC=<тека_з_rk_mpi_venc.h> \
     RKMPI_LIB=prebuilt \
     RGA_INC=../third_party/librga/include \
     RGA_LIB=prebuilt
# → build/thermal_stream
```
`third_party/librga/include/im2d.h` береться з каркаса `rv1106-upscale`
(або `scp` заголовків RGA із SDK).

### 4. На плату
```sh
scp build/thermal_stream root@192.168.50.2:/root/
```

## Запуск

**На платі** (спершу звільнити камеру й енкодер від rkipc):
```sh
ssh root@192.168.50.2 'pkill -9 rkipc ispserver 2>/dev/null; \
  LD_LIBRARY_PATH=/oem/usr/lib /root/thermal_stream --h265 -b 4000 -f 30'
```
Опції: `-d /dev/video21 -p 8080 -b <kbps> -g <gop> -f <fps> -W <w> -H <h> --h264|--h265`.
Застосунок слухає TCP :8080 і починає кодувати, щойно під'єднається плеєр.

**На ноуті** — плеєр з H.265-декодером (важливо: Fedora `ffmpeg-free` HEVC НЕ декодує,
див. розділ нижче):
```sh
# H.265:
ffplay -fflags nobuffer -flags low_delay -framedrop -f hevc tcp://192.168.50.2:8080
# H.264 (якщо запускав із --h264):
ffplay -fflags nobuffer -flags low_delay -framedrop -f h264 tcp://192.168.50.2:8080
```

## Декодер на ноуті (Fedora)

Стоковий Fedora `ffmpeg-free` зібраний **без H.264/H.265-декодерів**
(`--disable-decoder=h264,hevc`). Варіанти:

- **VLC** (свої кодеки, найпростіше):  `sudo dnf install vlc`, далі `… | vlc -`
- **Повний ffmpeg із RPM Fusion:**
  ```sh
  sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
  sudo dnf swap ffmpeg-free ffmpeg --allowerasing
  ```
- **Апаратний декод на GPU:** RTX 5060 має nvdec — `mpv --hwdec=nvdec …`.

## Статус

- [x] V4L2-захват YUYV, RGA YUYV→NV12, RK_MPI_VENC H.264/H.265, TCP-віддача — код готовий
- [ ] зібрати тулчейном (потрібен встановлений Luckfox SDK)
- [ ] звірити сигнатури RK_MPI під конкретну версію rockit на платі
- [ ] контраст термалки (AGC) — за потреби додати RGA/софт-стретч
- [ ] апаратний апскейл під дисплей — уже готово в коді (`-W/-H`), лишається офіційний екран
